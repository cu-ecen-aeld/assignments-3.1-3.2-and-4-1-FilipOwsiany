#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>

#define BUFFER_SIZE 256

static const char* version = "1.0.0";
static volatile sig_atomic_t stop = 0;

static int newSockFd = 0;
static int serverSockFd = 0;

static char* bufferPacket[128] = {NULL};
static uint8_t bufferPacketIndex = 0;

static bool run_as_daemon = false;

void bufferPacketFree(char **bufferPacket, uint8_t* bufferPacketIndex) 
{
    for (uint8_t i = 0; i < *bufferPacketIndex; i++) 
    {
        if (bufferPacket[i] != NULL) 
        {
            free(bufferPacket[i]);
            bufferPacket[i] = NULL;
        }
    }
    *bufferPacketIndex = 0;
}

void cleanup(void) 
{
    bufferPacketFree(bufferPacket, &bufferPacketIndex);
    if (serverSockFd > 0) 
    {
        close(serverSockFd);
    }
    if (newSockFd > 0) 
    {
        close(newSockFd);
    }
    syslog(LOG_INFO, "Server shutting down");
    remove("/var/tmp/aesdsocketdata");
    closelog();
    printf("Server shutting down\n");
}

void logAndExit(const char *msg, const char *filename, int exit_code) 
{
    char msgBuffer[256] = {0};
    if (strlen(msg) >= sizeof(msgBuffer) - 64) 
    {
        exit(EXIT_FAILURE);
    }
    snprintf(msgBuffer, sizeof(msgBuffer), "errno: %d msg: %s file: %s", errno, msg, filename ? filename : "unknown");
    syslog(LOG_ERR, "%s", msgBuffer);

    perror(msgBuffer);
    cleanup();
    exit(exit_code);
}

bool checkForNullCharInString(const char *str, const ssize_t len) 
{
    if (str == NULL || len <= 0) 
    {
        syslog(LOG_WARNING, "Received NULL string or invalid length");
        return true;
    }
    for (ssize_t i = 0; i < len; i++) 
    {
        if (str[i] == '\0') 
        {
            syslog(LOG_WARNING, "String contains null character at position %zd", i);
            return true;
        }
    }
    return false;
}

bool checkForNewlineCharInString(const char *str, const ssize_t len, ssize_t *newlinePosition) 
{
    if (str == NULL || len <= 0) 
    {
        syslog(LOG_DEBUG, "Received NULL string or invalid length");
        return true;
    }
    for (ssize_t i = 0; i < len; i++) 
    {
        if (str[i] == '\n') 
        {
            if (newlinePosition != NULL) 
            {
                *newlinePosition = i;
            }
            syslog(LOG_DEBUG, "String contains newline character at position %zd", i);
            return true;
        }
    }
    return false;
}

void SIGINTHandler(int signum, siginfo_t *info, void *extra)
{
    printf("Handler SIGINT, thread ID: %ld\n", syscall(SYS_gettid));
    stop = 1;
}

void SIGTERMHandler(int signum, siginfo_t *info, void *extra)
{
    printf("Handler SIGTERM, thread ID: %ld\n", syscall(SYS_gettid));
    stop = 1;
}

void setSignalSIGINTHandler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_flags = SIGINT;     
    action.sa_sigaction = SIGINTHandler;
    sigaction(SIGINT, &action, NULL);
}

void setSignalSIGTERMHandler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_flags = SIGTERM;     
    action.sa_sigaction = SIGTERMHandler;
    sigaction(SIGTERM, &action, NULL);
}

int main(int argc, char *argv[]) {
    printf("Server version: %s\n", version);
    openlog("Server", LOG_PID, LOG_USER);

    if (argc == 2 && strcmp(argv[1], "-d") == 0) 
    {
        run_as_daemon = true;
    }

    if (run_as_daemon) 
    {
        printf("Running as daemon\n");
        syslog(LOG_INFO, "Running as daemon");
        pid_t pid = fork();

        if (pid < 0) {
            logAndExit("Failed to fork for daemon", __FILE__, EXIT_FAILURE);
        }

        if (pid > 0) {
            // Rodzic wychodzi, dziecko działa w tle
            exit(EXIT_SUCCESS);
        }

        // Proces potomny staje się liderem sesji
        if (setsid() < 0) {
            logAndExit("Failed to setsid", __FILE__, EXIT_FAILURE);
        }

        // Zamknij standardowe deskryptory (opcjonalnie)
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd != -1) 
        {
            dup2(nullfd, STDIN_FILENO);
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            if (nullfd > STDERR_FILENO) 
            {
                close(nullfd);
            }
        }
    }

    setSignalSIGINTHandler();
    setSignalSIGTERMHandler();

    serverSockFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (serverSockFd < 0) {
        logAndExit("Failed to create socket", __FILE__, EXIT_FAILURE);
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;  
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);  
    serverAddr.sin_port = htons(9000);  

    if (bind(serverSockFd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) 
    {
        logAndExit("Failed to bind socket", __FILE__, EXIT_FAILURE);
    }

    listen(serverSockFd, 5);

    while (!stop)
    {
        struct sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);

        printf("Waiting for a connection...\n");
        newSockFd = accept(serverSockFd, (struct sockaddr *) &clientAddr, (socklen_t *) &clientLen);
        if (newSockFd < 0)
        {
            if (errno == EINTR && stop)
            {
                break;
            }
            logAndExit("Failed to accept connection", __FILE__, EXIT_FAILURE);
        }
        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(clientAddr.sin_addr));
        printf("Accepted connection from %s\n", inet_ntoa(clientAddr.sin_addr));

        while (1) 
        {
            char *buffer = (char*) calloc(BUFFER_SIZE, sizeof(char));

            if (buffer == NULL) 
            {
                logAndExit("Failed to allocate memory for buffer", __FILE__, EXIT_FAILURE);
            }

            if (bufferPacketIndex >= 128) 
            {
                bufferPacketFree(bufferPacket, &bufferPacketIndex);
                logAndExit("Buffer packet index exceeded limit", __FILE__, EXIT_FAILURE);
            }

            bufferPacket[bufferPacketIndex++] = buffer;

            ssize_t recvLen = recv(newSockFd, buffer, BUFFER_SIZE - 1, 0);
            if (recvLen >= 0 && recvLen < BUFFER_SIZE) {
                buffer[recvLen] = '\0';
            }

            if (recvLen < 0) 
            {
                bufferPacketFree(bufferPacket, &bufferPacketIndex);
                logAndExit("Failed to receive data", __FILE__, EXIT_FAILURE);
            } 
            else if (recvLen == 0) 
            {
                syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(clientAddr.sin_addr));
                printf("Closed connection from %s\n", inet_ntoa(clientAddr.sin_addr));
                bufferPacketFree(bufferPacket, &bufferPacketIndex);
                close(newSockFd);
                break;
            }      
            
            printf("Received %zd bytes from %s\n", recvLen, inet_ntoa(clientAddr.sin_addr));

            if (checkForNullCharInString(buffer, recvLen)) 
            {
                bufferPacketFree(bufferPacket, &bufferPacketIndex);
                printf("Received data contains null character\n");
            }

            ssize_t newlinePosition = -1;
            if (checkForNewlineCharInString(buffer, recvLen, &newlinePosition)) 
            {
                FILE *fptr;
                fptr = fopen("/var/tmp/aesdsocketdata", "a");

                printf("Received data contains newline character\n");
                for (size_t i = 0; i < bufferPacketIndex; i++)
                {
                    if (bufferPacket[i] == NULL) 
                    {
                        continue;
                    }
                    fwrite(bufferPacket[i], 1, strlen(bufferPacket[i]), fptr);
                }

                fclose(fptr);
                char bufferSend[BUFFER_SIZE] = {0};
                memset(bufferSend, 0, sizeof(bufferSend));

                
                fptr = fopen("/var/tmp/aesdsocketdata", "r");
                while (1)
                {
                    size_t bytesRead = fread(bufferSend, 1, sizeof(bufferSend), fptr);
                    if (bytesRead == 0) 
                    {
                        if (feof(fptr)) 
                        {
                            break;
                        } 
                        else 
                        {
                            logAndExit("Failed to read from file", __FILE__, EXIT_FAILURE);
                        }
                    }
                    send(newSockFd, bufferSend, bytesRead, 0);
                    memset(bufferSend, 0, sizeof(bufferSend));
                }
                fclose(fptr);
            }
        }
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    cleanup();    
    return EXIT_SUCCESS;
}
