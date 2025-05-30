//
// Created by arielxbp on 5/30/25.
//
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>

char* convertToBinary(char* k);
void init_client(char* serverAddr, int portno, char* binaryText, char* key);
char *cipherBinary(char* buffer, int threadCount, char *k);
char *readFile(char* filename);
char* initConcatBuf(char* k, int size, char* text);
void* execXOR(void* args);

typedef struct {
    int id;
    int resultStart;
    char* key;
    char* startP;
    char* result;
    int numBlocks;
} thread_args;

pthread_barrier_t barrier;
pthread_mutex_t lock;

static sigset_t criticalOperationSignals;
static int criticalSignalsInitialized = 0;

void initialize_critical_signals_set() {
    if (!criticalSignalsInitialized) {
        sigemptyset(&criticalOperationSignals);
        sigaddset(&criticalOperationSignals, SIGINT);
        sigaddset(&criticalOperationSignals, SIGALRM);
        sigaddset(&criticalOperationSignals, SIGUSR1);
        sigaddset(&criticalOperationSignals, SIGUSR2);
        sigaddset(&criticalOperationSignals, SIGTERM);
        criticalSignalsInitialized = 1;
    }
}

int main(int argc, char *argv[]) {
    char* fileName;
    int p; // number of threads
    char* k; // key for XOR
    char* serverAddr;
    int portno;

    int option;

    while ((option = getopt(argc, argv, "f:p:k:s:n:")) != -1) {
        switch (option) {
            case 'f':
                fileName = optarg;
                break;
            case 'p':
                p = atoi(optarg);
                break;
            case 'k':
                k = optarg;
                break;
            case 's':
                serverAddr = optarg;
                break;
            case 'n':
                portno = atoi(optarg);
                break;
            default:
                perror("missing arguments");
        }
    }

    // Obtain the file's text
    char* text = readFile(fileName);
    // Obtain the key for XOR
    char* key = convertToBinary(k);

    char* buffer = initConcatBuf(key, strlen(text), text);


    sigset_t old_mask_cipher;
    printf("CLIENT: Blocking signals before ciphering process\n");
    if (sigprocmask(SIG_BLOCK, &criticalOperationSignals, &old_mask_cipher) < 0) {
        perror("CLIENT: ERROR sigprocmask (block for cipher)");
        exit(1);
    }

    char* binaryText = cipherBinary(buffer, p, key);
    init_client(serverAddr, portno, binaryText, key);

    printf("CLIENT: Restoring signals after ciphering process\n");
    if (sigprocmask(SIG_SETMASK, &old_mask_cipher, NULL) < 0) {
        perror("CLIENT: ERROR sigprocmask (restore after cipher)");
    }
    printf("CLIENT: Restored signals\n");

    // Free allocated memory
    free(text);
    free(key);
    free(buffer);
    free(binaryText);
}

char* readFile(char* filename) {
    FILE *fp;
    int filelen;
    char *buf;
    // Open the file in binary mode
    fp = fopen(filename, "rb");
    if (fp == NULL) {
        perror("Error opening file");
        exit(0);
    }

    // Get the length of the file
    fseek(fp, 0, SEEK_END);
    filelen = ftell(fp); // Get length of file in bytes
    fseek(fp, 0, SEEK_SET);

    // Allocate memory for the buffer
    buf = (char *)malloc(filelen + 1); // 8 bits per byte + null terminator
    if (buf == NULL) {
        perror("Memory allocation failed");
        fclose(fp);
        return NULL;
    }

    // Read the file into the buffer
    fread(buf, 1, filelen, fp);
    buf[filelen] = '\0'; // Null-terminate the string

    fclose(fp);
    return buf;
}

char* initConcatBuf(char* key, int size, char* text) {

    char* binaryText = convertToBinary(text);

    int textLenBeforePadding = strlen(binaryText);
    int remainder = 0;
    int padding = 0;

    if ((textLenBeforePadding)%64 != 0) {
        remainder = textLenBeforePadding % 64; // 176 % 64 = 48
        padding = 64 - remainder; // 64 - 48 = 16
    }

    if (padding > 0) {
        char* temp_text = realloc(binaryText, textLenBeforePadding + padding + 1);
        if (temp_text == NULL) {
            perror("Memory reallocation failed");
            free(binaryText);
            exit(1);
        }
        binaryText = temp_text;
        for (int i = 0; i < padding; i++) {
            binaryText[textLenBeforePadding + i] = '0';
        }
        binaryText[textLenBeforePadding + padding] = '\0';
    }

    printf("CLIENT: Full binary for key: \n%s (length %zu)\n", key, strlen(key));
    printf("CLIENT: Full binary for padded text: \n%s (length %zu)\n", binaryText, strlen(binaryText));

    return binaryText;

}

char *cipherBinary( char* buffer ,int threadCount, char *key) {

    int len = strlen(buffer);
    char* result = (char*)calloc(len+1, sizeof(char));

    int numBlocks = len / 64; // 176 / 64 = 2
    int numBlocksForThread = numBlocks / threadCount; // 3 / 2 = 1
    int remainingBlocks = numBlocks % threadCount; // 3 % 2 = 1
    int lastPointer = 0; // Last position used for a thread

    // If the number of threads is greater than the number of blocks, adjust threadCount
    if (threadCount > numBlocks) {
        threadCount = numBlocks;
    }

    pthread_t threads[threadCount];

    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("Mutex init failed");
        free(result);
        free(buffer);
        exit(1);
    }

    if (pthread_barrier_init(&barrier, NULL, threadCount) != 0) {
        perror("Failed to initialize barrier");
        free(result);
        free(buffer);
        exit(1);
    }

    for (int i = 0; i < threadCount; i++) {

        // Allocate memory for thread arguments
        thread_args *args = malloc(sizeof(thread_args));
        if (!args) {
            perror("Failed to allocate thread arguments");
            exit(1);
        }
        if (remainingBlocks>0) {
            // If there are remaining blocks, assign one more block to this thread
            args->numBlocks = numBlocksForThread + 1;
            remainingBlocks--;
        } else {
            args->numBlocks = numBlocksForThread;
        }

        char *startP = &buffer[lastPointer]; // Buffer + lastPointer
        lastPointer = startP - buffer;

        args->id = i;
        args->key = key;
        args->result = result;
        args->startP = buffer+lastPointer; // Start position for this thread
        args->resultStart = lastPointer; // Start position in result for this thread
        lastPointer += (args->numBlocks * 64); // Update last pointer for next thread

        // Assign for each thread a segment of the buffer to work on using execXOR
        if (pthread_create(&threads[i], NULL, execXOR, args) != 0) {
            perror("Failed to create thread");
            exit(1);
        }
    }

    for (int i = 0; i < threadCount; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("Failed to join thread");
            exit(1);
        }
    }

    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&lock);


    printf("CLIENT: All threads have finished\n");
    printf("CLIENT: (Ciphered text (binary, len %d)):\n%s\n", len, result);

    return result;
}

void* execXOR(void* args) {
    thread_args *t_args = (thread_args *)args;
    int id = t_args->id;
    char* key = t_args->key;
    char* startP = t_args->startP;
    pthread_mutex_lock(&lock);
    char* result = t_args->result;
    pthread_mutex_unlock(&lock);
    int numBlocks = t_args->numBlocks;
    int resultStart = t_args->resultStart;

    for (int i = 0; i < numBlocks; i++) {
        for (int j = 0; j < 64; j++) {
            pthread_mutex_lock(&lock);
            if (key[j]==startP[j+i*64]) {
                result[resultStart+i*64+j] = '0'; // 128 + i*64 + j
            } else {
                result[id*numBlocks+i*64+j] = '1';
            }
            pthread_mutex_unlock(&lock);
        }
    }

    free(t_args); // Free allocated memory for thread arguments

    int s =pthread_barrier_wait(&barrier);  // All threads block here until NUM_THREADS arrive
    if (s!= 0 && s != PTHREAD_BARRIER_SERIAL_THREAD) {
        perror("Barrier wait failed");
        exit(1);
    }
}


char* convertToBinary(char* k) {

    int len = strlen(k);
    int bitsLen = len * 8;
    char* binary = (char*)malloc(bitsLen+1); // 64 bits + '\0'

    // Convert each of the first 8 bytes to binary
    for (int i = 0; i < len; i++) {
        unsigned char c = k[i]; // Pad with '\0' if k < 8 bytes
        for (int j = 0; j < 8; j++) {
            binary[i * 8 + j] = (c & (1 << (7 - j))) ? '1' : '0';
        }
    }
    binary[bitsLen] = '\0'; // Null-terminate the string
    //printf("\nFull binary: %s\n", binary);
    return binary;
}



void init_client(char* serverAddr, int portno, char* binaryText, char* key) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct in_addr addr;

    // Length of the binary text
    int binaryTextLen = strlen(binaryText);
    char binaryTextLenStr[20];
    bzero(binaryTextLenStr, sizeof(binaryTextLenStr)); // To clean the buffer
    snprintf(binaryTextLenStr, sizeof(binaryTextLenStr), "%d", binaryTextLen);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(0);
    }

    inet_aton(serverAddr, &addr);
    server = gethostbyaddr(&addr, sizeof(addr), AF_INET);
    bcopy((char *)server->h_addr_list[0], (char *)&serv_addr.sin_addr.s_addr, server->h_length);

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);

    if (connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0) {
        perror("ERROR connecting");
        exit(0);
    }

    int n = 0;
    n = write(sockfd, binaryTextLenStr, sizeof(binaryTextLenStr));
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(0);
    }
    n = write(sockfd, binaryText, strlen(binaryText));
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(0);
    }
    n = write(sockfd, key, 64);
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(0);
    }

    char buffer[256];
    bzero(buffer,256);
    n = read(sockfd,buffer,255);
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(0);
    }
    printf("%s",buffer);

}


    // bzero(buffer,256);
    // n = read(sockfd,buffer,255);
    // if (n < 0)
    //     perror("ERROR reading from socket");
    //     exit(0);
    // printf("%s",buffer);


// Alternativa per ottenere l'indirizzo IP
// if (inet_pton(AF_INET, serverAddr, &serv_addr.sin_addr) <= 0) {
//     server = gethostbyname(serverAddr);
//     bcopy((char *)server->h_addr_list[0], (char *)&serv_addr.sin_addr.s_addr, server->h_length);
// }

// Example of input from the user
// char buffer[256];
// bzero(buffer,256); // Sets 256 bytes to 0 in buffer like a calloc
// printf("Please enter the message: ");
// fgets(buffer,255,stdin);