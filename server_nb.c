#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>


void init_server(int max_threads, char* prefix, int connection_num);
void readfile(int sockfd, int thread_num, char *string);
void* execXOR(void* args);
char* processBinaryText(uint8_t* asciiText, int binaryTextLen, int textSize, char* key, int threadCount);

const int debug = 1; // Global debug flag, set to 1 for debugging output

typedef struct BlockNode {
    char block[8];
    struct BlockNode *next; // Pointer to the next block in the linked list
} BlockNode;

typedef struct {
    int id;
    char* key;
    int realSize;
    uint8_t* startP;
    BlockNode* result;
    int numBlocks;
} thread_args;

pthread_mutex_t lock;

int main(int argc, char *argv[]) {
    int p = 2;
    int l = 5;
    char* s = "miofile";
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
    int portno = 5430;
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

void writeFile(char* resultText, char* string) {

    char outputFileName[256];
    snprintf(outputFileName, sizeof(outputFileName), "%s.txt", string);
    FILE *outputFile = fopen(outputFileName, "w");
    if (outputFile == NULL) {
        perror("Failed to open output file");
        free(resultText);
        exit(1);
    }
    printf("SERVER: Writing in: %s.txt\n", string);
    fprintf(outputFile, "%s", resultText);
    fclose(outputFile);
    printf("SERVER: Result written to %s\n", outputFileName);
}

void readfile(int sockfd, int thread_num, char *string) {
    int n;
    char* prefix = string;
    char binaryTextLenStr[20];
    bzero(binaryTextLenStr,20);
    n = read(sockfd, binaryTextLenStr, sizeof(binaryTextLenStr));
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    long int binaryTextLen = atoi(binaryTextLenStr);
    int paddedsize = ((binaryTextLen/8) % 8 == 0) ? binaryTextLen/8 : binaryTextLen/8 + (8-(binaryTextLen/8)%8);
    uint8_t binaryText[paddedsize];

    n = read(sockfd, binaryText, sizeof(binaryText));
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }
    char key[8];
    n = read(sockfd, key, 8);

    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    /*if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): The binaryTextLen is: %s\n", pid, binaryTextLenStr);
        printf("SERVER (Child %d): The binaryText is: %s\n", pid, binaryText);
        printf("SERVER (Child %d): The key is: %s\n", pid, key);
    }*/

    char* resultText = processBinaryText(binaryText, sizeof(binaryText), binaryTextLen, key, thread_num);

    // Send a response back to the client
    n = write(sockfd,"SERVER: Correctly received ciphered text\n",41);
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(1);
    }
    fflush(stdout);
    usleep(100000);

    close(sockfd);

    // Write the result to a file
    writeFile(resultText, prefix);
    free(resultText);
}

char* processBinaryText(uint8_t* asciiText, int binaryTextLen, int textSize, char* key, int threadCount) {

    int numTotalBlocks = binaryTextLen / 8;

    // Allocate memory for managing threads and their arguments
    pthread_t *threads = malloc(threadCount * sizeof(pthread_t)); // Array of thread IDs
    thread_args **args_array = malloc(threadCount * sizeof(thread_args*)); // Array of pointers to thread_args structs
    if (!threads || !args_array) {
        perror("SERVER: Failed to allocate memory for thread management structures");
        // Free any allocated memory
        if(threads) free(threads);
        if(args_array) free(args_array);
        exit(1);
    }

    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("Mutex init failed");
        free(threads);
        free(args_array);
        exit(1);
    }


    // Initialize the result linked list
    BlockNode* result_list_head = NULL;
    BlockNode* result_list_tail = NULL;

    int blocksPerThread_base = numTotalBlocks / threadCount;
    int remainingBlocks_distribute = numTotalBlocks % threadCount;
    int currentInputOffset = 0;
    /*
    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): Total blocks: %d, Blocks per thread (base): %d, Remaining blocks to distribute: %d\n", pid, numTotalBlocks, blocksPerThread_base, remainingBlocks_distribute);
    }*/

    for (int i = 0; i < threadCount; i++) {

        args_array[i] = malloc(sizeof(thread_args));
        if (!args_array[i]) {
            perror("SERVER: Malloc failed for thread_args");
            exit(1);
        }
        thread_args *current_args = args_array[i];

        current_args->id = i;
        current_args->key = key;
        current_args->realSize = textSize;
        current_args->numBlocks = blocksPerThread_base;
        if (remainingBlocks_distribute > 0) {
            current_args->numBlocks++;
            remainingBlocks_distribute--;
        }
        current_args->startP = asciiText + currentInputOffset;
        currentInputOffset += (current_args->numBlocks * 8);

        BlockNode* localFirstNode = NULL;
        BlockNode* localCurrentNewNode = NULL;

        for (int j = 0; j < current_args->numBlocks; j++) {
            BlockNode *newNode = (BlockNode *)malloc(sizeof(BlockNode));
            if (!newNode) {
                perror("Failed to allocate memory for block node");
                free(current_args);
                exit(1);
            }

            memset(newNode->block, 0, 8); // Initialize, e.g. with 'X' for debugging
            newNode->next = NULL;

            if (localFirstNode == NULL) {
                localFirstNode = newNode;
            }
            if (localCurrentNewNode != NULL) {
                localCurrentNewNode->next = newNode;
            }
            localCurrentNewNode = newNode;

            // Append to the overall list
            if (result_list_head == NULL) {
                result_list_head = newNode;
                result_list_tail = newNode;
            } else {
                result_list_tail->next = newNode;
                result_list_tail = newNode;
            }
        }
        current_args->result = localFirstNode; // Head of this thread's output list
        /*
        if (debug==1) {
            int pid = getpid();
            printf("SERVER (Child %d): Thread %d: numBlocks=%d, input_offset=%ld\n", pid, i, current_args->numBlocks, (current_args->startP - binaryText));
        }*/

        if (pthread_create(&threads[i], NULL, execXOR, current_args) != 0) {
            perror("Failed to create thread");
            free(current_args);
            exit(1);
        }
    }

    for (int i = 0; i < threadCount; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("Failed to join thread");
            exit(1);
        }
    }

    // Concatenate results from all thread_result_heads
    char* finalResultBuffer = (char*)malloc(numTotalBlocks * 8);
    if (!finalResultBuffer) {
        perror("malloc for final_result_buffer failed");
        exit(1);
    }
    finalResultBuffer[0] = '\0'; // Start with an empty string for strcat

    BlockNode* currentBlockToConcat = result_list_head;
    while (currentBlockToConcat != NULL) {
        strncat(finalResultBuffer, currentBlockToConcat->block, 8);
        currentBlockToConcat = currentBlockToConcat->next;
    }

    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): Resulting binary text: %s\n", pid, finalResultBuffer);
        fflush(stdout);
    }

    // Destroy the mutex
    pthread_mutex_destroy(&lock);

    // Free management structures
    free(threads);
    free(args_array);
    return finalResultBuffer;
}

void* execXOR(void* args) {
    thread_args *t_args = (thread_args *)args;
    char* key = t_args->key;
    uint8_t* startP = t_args->startP;
    pthread_mutex_lock(&lock);
    BlockNode* currentResultNode = t_args->result;
    pthread_mutex_unlock(&lock);
    int threadNum = t_args->numBlocks;
    int realSize = t_args->realSize;

    for (int i = 0; i < threadNum; i++) {
        for (int j = 0; j < 8; j++) {
            pthread_mutex_lock(&lock);
            if (i*8+j<realSize) {
                currentResultNode->block[j] = key[j]^startP[i * 8 + j];
            } else {
                currentResultNode->block[j] = 0;
            }
            pthread_mutex_unlock(&lock);
        }
        currentResultNode = currentResultNode->next; // Move to the next BlockNode for the next output block
    }
    free(t_args);
}
