#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>

void init_server(int max_threads, char* prefix, int connection_num);
void readfile(int sockfd, int thread_num, char *string);

typedef struct {
    int id;
    int resultStart;
    char* key;
    char* startP;
    char* result;
    int numBlocks;
} thread_args;

int main(int argc, char *argv[]) {
    int p, l;
    char* s;
    int option;

    while ((option = getopt(argc, argv, "t:p:c:")) != -1) {
        switch (option) {
            case 't':
                p = atoi(optarg);
                break;
            case 'p':
                s = optarg;
                break;
            case 'c':
                l = atoi(optarg);
                break;
            default:
                perror("missing arguments");
        }
    }

    init_server(p, s, l);
}


void init_server(int max_threads, char* prefix, int connection_num) {
    int sockfd, newsockfd, pid;
    int portno = 5431;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;


    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    serv_addr.sin_addr.s_addr = INADDR_ANY;


    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR on binding");
        exit(1);
    }

    listen(sockfd, 5);

    printf("SERVER: Listening on port %d...\n", portno);
    clilen = sizeof(cli_addr);

    while (1) {
        if (connection_num > 0) {
            newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
            connection_num--;
            printf("SERVER: Connection accepted, remaining connections: %d\n", connection_num);
            if (newsockfd < 0) {
                perror("ERROR on accept");
                exit(1);
            }

            pid = fork();
            if (pid < 0) {
                perror("ERROR on fork");
                exit(1);
            }
            if (pid == 0) {
                close(sockfd);
                // Here it does something
                printf("SERVER (Child %d): Handling connection\n", getpid());
                readfile(newsockfd, max_threads, prefix);
                exit(0);
            }
            else {
                close(newsockfd);
                waitpid(pid, NULL, WNOHANG);
                connection_num++;
            }
        }
    }
}

void readfile(int sockfd, int thread_num, char *string) {
    int n;

    char binaryTextLenStr[20];
    bzero(binaryTextLenStr,20);
    n = read(sockfd, binaryTextLenStr, sizeof(binaryTextLenStr));
    int binaryTextLen = atoi(binaryTextLenStr);

    char binaryText[binaryTextLen];
    n = read(sockfd, binaryText, sizeof(binaryText));
    binaryText[n] = '\0';

    char key[64];
    n = read(sockfd, key, sizeof(key));
    key[n] = '\0';

    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    int pid = getpid();
    printf("SERVER (Child %d): The binaryTextLen is: %s\n", pid, binaryTextLenStr);
    printf("SERVER (Child %d): The binaryText is: %s\n", pid, binaryText);
    printf("SERVER (Child %d): The key is: %s\n", pid, key);

    // Send a response back to the client
    n = write(sockfd,"SERVER: Correctly received ciphered text\n",41);
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(1);
    }
    fflush(stdout);
    usleep(100000);
    close(sockfd);
}





