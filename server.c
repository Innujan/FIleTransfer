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
char* processBinaryText(char* binaryText, char* key, int threadCount);

const int debug = 1; // Global debug flag, set to 1 for debugging output

typedef struct BlockNode {
    char block[65];
    struct BlockNode *next; // Pointer to the next block in the linked list
} BlockNode;

typedef struct {
    int id;
    char* key;
    char* startP;
    BlockNode* result;
    int numBlocks;
} thread_args;

pthread_mutex_t lock;

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

char* convertToString(char* text) {

    int binaryLen = strlen(text);

    int charCount = binaryLen / 8;
    char* result = malloc(charCount + 1); // +1 for null terminator
    if (!result) {
        perror("Memory allocation failed");
        free(text);
        return NULL;
    }

    for (int i = 0; i < charCount; i++) {
        int byte = 0;
        char* byteStart = text + (i * 8);

        // Convert 8 binary digits to a byte
        for (int bit = 0; bit < 8; bit++) {
            if (byteStart[bit] == '1') {
                byte |= (1 << (7 - bit));
            }
            else if (byteStart[bit] != '0') {
                fprintf(stderr, "Error: Invalid binary digit '%c'\n", byteStart[bit]);
                free(result);
                return NULL;
            }
        }
        result[i] = (char)byte;
    }

    result[charCount] = '\0'; // Null-terminate
    return result;
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
    fprintf(outputFile, "%s", resultText);
    fclose(outputFile);
    free(resultText);
    free(string);
    if (debug==1) {
        printf("SERVER: Result written to %s\n", outputFileName);
    }

}

void readfile(int sockfd, int thread_num, char *string) {
    int n;
    char* prefix = string;
    char binaryTextLenStr[20];
    bzero(binaryTextLenStr,20);
    n = read(sockfd, binaryTextLenStr, sizeof(binaryTextLenStr));
    int binaryTextLen = atoi(binaryTextLenStr);
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    char binaryText[binaryTextLen];
    n = read(sockfd, binaryText, sizeof(binaryText));
    binaryText[n] = '\0';
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    char key[64];
    n = read(sockfd, key, sizeof(key));
    key[n] = '\0';

    if (n < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): The binaryTextLen is: %s\n", pid, binaryTextLenStr);
        printf("SERVER (Child %d): The binaryText is: %s\n", pid, binaryText);
        printf("SERVER (Child %d): The key is: %s\n", pid, key);
    }

    char* resultText = processBinaryText(binaryText, key, thread_num);
    resultText = convertToString(resultText);

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
}

char* processBinaryText(char* binaryText, char* key, int threadCount) {

    int binaryTextLen = strlen(binaryText);
    int numTotalBlocks = binaryTextLen / 64;

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

    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): Total blocks: %d, Blocks per thread (base): %d, Remaining blocks to distribute: %d\n", pid, numTotalBlocks, blocksPerThread_base, remainingBlocks_distribute);
    }

    for (int i = 0; i < threadCount; i++) {

        args_array[i] = malloc(sizeof(thread_args));
        if (!args_array[i]) {
            perror("SERVER: Malloc failed for thread_args");
            exit(1);
        }
        thread_args *current_args = args_array[i];

        current_args->id = i;
        current_args->key = key;
        current_args->numBlocks = blocksPerThread_base;
        if (remainingBlocks_distribute > 0) {
            current_args->numBlocks++;
            remainingBlocks_distribute--;
        }
        current_args->startP = binaryText + currentInputOffset;
        currentInputOffset += (current_args->numBlocks * 64);

        BlockNode* localFirstNode = NULL;
        BlockNode* localCurrentNewNode = NULL;

        for (int j = 0; j < current_args->numBlocks; j++) {
            BlockNode *newNode = (BlockNode *)malloc(sizeof(BlockNode));
            if (!newNode) {
                perror("Failed to allocate memory for block node");
                free(current_args);
                exit(1);
            }

            memset(newNode->block, 0, 64); // Initialize, e.g. with 'X' for debugging
            newNode->block[64] = '\0';      // Null terminate
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

        if (debug==1) {
            int pid = getpid();
            printf("SERVER (Child %d): Thread %d: numBlocks=%d, input_offset=%ld\n", pid, i, current_args->numBlocks, (current_args->startP - binaryText));
        }

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
    char* finalResultBuffer = (char*)malloc(numTotalBlocks * 64 + 1);
    if (!finalResultBuffer) {
        perror("malloc for final_result_buffer failed");
        exit(1);
    }
    finalResultBuffer[0] = '\0'; // Start with an empty string for strcat

    BlockNode* currentBlockToConcat = result_list_head;
    while (currentBlockToConcat != NULL) {
        strncat(finalResultBuffer, currentBlockToConcat->block, 64);
        BlockNode* temp = currentBlockToConcat;
        currentBlockToConcat = currentBlockToConcat->next;
        free(temp);
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
    char* startP = t_args->startP;
    pthread_mutex_lock(&lock);
    BlockNode* currentResultNode = t_args->result;
    pthread_mutex_unlock(&lock);
    int threadNum = t_args->numBlocks;
    int keyLen = strlen(key);

    for (int i = 0; i < threadNum; i++) {
        for (int j = 0; j < 64; j++) {
            if (key[j % keyLen] == startP[i * 64 + j]) {
                pthread_mutex_lock(&lock);
                currentResultNode->block[j] = '0';
            } else {
                currentResultNode->block[j] = '1';
            }
            pthread_mutex_unlock(&lock);
        }
        currentResultNode->block[64] = '\0'; // Null-terminate this specific block's string
        currentResultNode = currentResultNode->next; // Move to the next BlockNode for the next output block
    }
    free(t_args);
}
