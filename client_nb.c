#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>

char* convertToBinary(char* k);
void init_client(char* serverAddr, int portno, uint8_t* asciiText, int paddedsize, long int size, char* key);
void* cipherBinary(char* text, int threadCount, char *k, uint8_t* buffer, int size, int paddedsize);
char *readFile(char* filename);
void* execXOR(void* args);

typedef struct {
    int id;
    int resultStart;
    char* key;
    char* startP;
    char* result;
    int numBlocks;
    int realSize;
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
    int p = 2; // number of threads
    char* key; // key for XOR
    char* serverAddr;
    int portno;

    if (argc < 5) {
        perror("CLIENT: ERROR missing arguments");
        exit(1);
    }
    fileName = argv[1];
    key = argv[2];
    serverAddr = argv[3];
    portno = atoi(argv[4]);
    int option;

    while ((option = getopt(argc, argv, "p:")) != -1) {
        switch (option) {
            case 'p':
                p = atoi(optarg);
                break;
            default:
                perror("missing arguments");
        }
    }

    // Obtain the file's text
    char* text = readFile(fileName);
    long int textSize = strlen(text)*8;

    sigset_t old_mask_cipher;
    printf("CLIENT: Blocking signals before ciphering process\n");
    if (sigprocmask(SIG_BLOCK, &criticalOperationSignals, &old_mask_cipher) < 0) {
        perror("CLIENT: ERROR sigprocmask (block for cipher)");
        exit(1);
    }

    int size = strlen(text);
    int paddedsize = (size % 8 == 0) ? size : size + (8-size % 8);
    uint8_t asciiText[paddedsize];

    cipherBinary(text, p, key, asciiText, size, paddedsize);
    init_client(serverAddr, portno, asciiText, paddedsize, textSize, key);

    printf("CLIENT: Restoring signals after ciphering process\n");
    if (sigprocmask(SIG_SETMASK, &old_mask_cipher, NULL) < 0) {
        perror("CLIENT: ERROR sigprocmask (restore after cipher)");
    }
    printf("CLIENT: Restored signals\n");

    // Free allocated memory
    free(text);
}

char* readFile(char* filename) {
    int fd;
    long filelen;
    char *buf;
    // Open the file in binary mode
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Error opening file");
        exit(0);
    }

    // Get the length of the file
    filelen = lseek(fd, 0, SEEK_END); // Get length of file in bytes
    lseek(fd, 0, SEEK_SET);

    // Allocate memory for the buffer
    buf = (char *)calloc(filelen+1, sizeof(char)); // 8 bits per byte + null terminator
    if (buf == NULL) {
        perror("Memory allocation failed");
        fclose(fd);
        return NULL;
    }

    // Read the file into the buffer
    read(fd, buf, filelen);

    close(fd);
    return buf;
}

void* cipherBinary(char* text, int threadCount, char *key, uint8_t* buffer, int size, int paddedsize) {

    int numBlocks = paddedsize / 8; // 176 / 64 = 2
    int numBlocksForThread = numBlocks / threadCount; // 3 / 2 = 1
    int remainingBlocks = numBlocks % threadCount; // 3 % 2 = 1
    int lastPointer = 0;

    if (threadCount > numBlocks) {
        threadCount = numBlocks;
    }

    pthread_t threads[threadCount];

    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("Mutex init failed");
        free(buffer);
        exit(1);
    }

    if (pthread_barrier_init(&barrier, NULL, threadCount) != 0) {
        perror("Failed to initialize barrier");
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

        char *startP = &text[lastPointer]; // Buffer + lastPointer
        lastPointer = startP - text;

        args->id = i;
        args->key = key;
        args->result = buffer;
        args->startP = text+lastPointer; // Start position for this thread
        args->resultStart = lastPointer; // Start position in result for this thread
        args->realSize = size;
        lastPointer += (args->numBlocks*8); // Update last pointer for next thread

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
}

void* execXOR(void* args) {
    thread_args *t_args = (thread_args *)args;
    char* key = t_args->key;
    char* startP = t_args->startP;
    pthread_mutex_lock(&lock);
    uint8_t* buffer = t_args->result;
    pthread_mutex_unlock(&lock);
    int numBlocks = t_args->numBlocks;
    int realSize = t_args->realSize;
    int resultStart = t_args->resultStart;

    for (int i = 0; i < numBlocks; i++) {
        for (int j = 0; j < 8; j++) {
            pthread_mutex_lock(&lock);
            if (j+i*8 < realSize) {
                buffer[resultStart+j+i*8] = startP[j+i*8]^key[j];
            }
            else {
                buffer[resultStart+j+i*8] = 0;
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
    return binary;
}

void init_client(char* serverAddr, int portno, uint8_t* asciiText, int paddedsize, long int size, char* key) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct in_addr addr;

    // Length of the binary text
    int binaryTextLen = size;
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
    n = write(sockfd, asciiText, paddedsize);
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(0);
    }
    n = write(sockfd, key, 8);
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(0);
    }

    // Read the response from the server
    char buffer[256];
    bzero(buffer,256);
    n = read(sockfd,buffer,255);
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(0);
    }
    printf("%s", buffer);
    close(sockfd);
}