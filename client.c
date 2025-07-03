#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

char* convertToBinary(char* string);
void init_client(char* serverAddr, int portno, char* binaryText, char* key);
char *cipherBinary(char* buffer, int threadCount, char *k);
char *readFile(char* filename);
char* initPaddedText(char* text);
void* execXOR(void* args);

/**
 * Variabile globale per il debug, impostata a 1 per avere sullo stdout le informazioni di debug
 */
const int debug = 1;

/**
 * Struttura per passare gli argomenti ai thread
 */
typedef struct {
    int id;
    int resultStart;
    char* key;
    char* startP;
    char* result;
    int numBlocks;
} thread_args;

/**
 * Barriera per sincronizzare i thread
 */
pthread_barrier_t barrier;

/**
 * Mutex per la mutua esclusione tra i thread
 */
pthread_mutex_t lock;

/**
 * Set di segnali critici da bloccare durante l'operazione di cifratura
 */
static sigset_t criticalOperationSignals;

/**
 * Flag per verificare se il set di segnali è stato inizializzato
 */
static int criticalSignalsInitialized = 0;

/**
 * Inizializza il set di segnali critici da bloccare durante l'operazione di cifratura
 */
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

/**
 * Funzione main del server
 */
int main(int argc, char *argv[]) {
    char* fileName;
    int threads; // Determina il numero di parallelismo dell'operazione di cifratura
    char* k; // Chiave per l'operazione di XOR
    char* serverAddr; // Indirizzo del server
    int portno; // Porta del server

    int option; //

    while ((option = getopt(argc, argv, "f:p:k:s:n:")) != -1) { // f: file name, p: threads, k: key, s: server address, n: port number
        switch (option) {
            case 'f':
                fileName = optarg;
                break;
            case 'p':
                // threads = atoi(optarg); // atoi non gestisce gli errori -> usiamo strtol
                errno = 0;
                threads = (int)strtol(optarg, NULL, 10);
                if (errno == ERANGE) {
                    perror("strtol");
                    exit(EXIT_FAILURE); // Stessa cosa di exit(1);
                }
                break;
            case 'k':
                k = optarg;
                break;
            case 's':
                serverAddr = optarg;
                break;
            case 'n':
                errno = 0;
                portno = (int)strtol(optarg, NULL, 10);
                if (errno == ERANGE) {
                    perror("strtol");
                    exit(EXIT_FAILURE);
                }
                break;
            default:
                perror("missing arguments");
        }
    }

    // Ottengo il testo da cifrare dal file
    char* text = readFile(fileName);

    // Ottengo la chiave in binario per l'operazione di XOR
    char* key = convertToBinary(k);

    // Si aggiunge al testo da cifrare un padding se necessario per renderlo un multiplo di 64 bit
    char* buffer = initPaddedText(text);

    if (debug==1) {
        printf("CLIENT: Full binary for key: \n%s (length %zu)\n", key, strlen(key));
        printf("CLIENT: Full binary for padded text: \n%s (length %zu)\n", buffer, strlen(buffer));
    }

    // Salva il vecchio set di segnali per poterlo ripristinare dopo l'operazione di cifratura e di invio
    sigset_t old_mask_cipher;

    if (debug==1) {
        printf("CLIENT: Blocking signals before ciphering process\n");
    }

    // Cambia il set di segnali per bloccare i segnali critici interessati durante l'operazione di cifratura e di invio
    if (sigprocmask(SIG_BLOCK, &criticalOperationSignals, &old_mask_cipher) == -1) {
        perror("CLIENT: ERROR sigprocmask (block for cipher)");
        free(text);
        free(key);
        free(buffer);
        exit(EXIT_FAILURE);
    }

    // Cifra il testo binario tramite XOR con la chiave
    char* binaryText = cipherBinary(buffer, threads, key);

    // Inizializza il client per inviare il testo cifrato al server
    init_client(serverAddr, portno, binaryText, key);

    if (debug==1) {
        printf("CLIENT: Restoring signals after sending ciphered text to server\n");
    }

    // Ripristina il set di segnali precedente al blocco
    if (sigprocmask(SIG_SETMASK, &old_mask_cipher, NULL) == -1) {
        perror("CLIENT: ERROR sigprocmask (restore after cipher)");
        free(text);
        free(key);
        free(buffer);
        free(binaryText);
        exit(EXIT_FAILURE);
    }

    if (debug==1) {
        printf("CLIENT: Restored signals\n");
    }

    // Free allocated memory
    free(text);
    free(key);
    free(buffer);
    free(binaryText);
}

/**
 * Funzione per leggere il file e restituirne il contenuto come stringa
 * @param filename: nome del file da leggere
 * @return: puntatore alla stringa contenente il testo del file
 */
char* readFile(char* filename) {
    FILE *fp;
    long fileLen;
    char *buf;

    fp = fopen(filename, "rb"); // Open the file in binary mode
    if (fp == NULL) {
        perror("ERROR on opening file");
        exit(EXIT_FAILURE);
    }

    // Per ottenere la lunghezza del file
    fseek(fp, 0, SEEK_END); // Muovi il puntatore del file alla fine del file per il ftell
    fileLen = ftell(fp); // ftell restituisce la posizione corrente del puntatore del file in bytes
    fseek(fp, 0, SEEK_SET); // Rimetti il puntatore del file alla posizione iniziale

    // Alloca memoria per il buffer
    buf = (char *)malloc(fileLen + 1); // 8 bits per byte + null terminator
    if (buf == NULL) {
        perror("ERROR: Memory allocation failed");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    // Legge il file per memorizzare il contenuto nel buffer
    int bytesRead;
    bytesRead = (int)fread(buf, 1, fileLen, fp);
    if (bytesRead < fileLen) {
        perror("ERROR: fread failed");
        free(buf);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    buf[fileLen] = '\0'; // Null-termino il buffer

    fclose(fp);

    return buf;
}

/**
 * Funzione per inizializzare il testo da cifrare con un padding se necessario
 * @param size: la dimensione del testo
 * @param text: il testo da cifrare
 * @return: il testo binario modificato con un padding se necessario
 */
char* initPaddedText(char* text) {

    // Converte il testo in binario
    char* binaryText = convertToBinary(text);
    int textLenBeforePadding = (int)strlen(binaryText);
    int remainder;
    int padding = 0;

    if ((textLenBeforePadding)%64 != 0) {
        remainder = textLenBeforePadding % 64; // e.g. 176 % 64 = 48
        padding = 64 - remainder; // e.g. 64 - 48 = 16
    }

    if (padding > 0) {
        char* temp_text = (char*)realloc(binaryText, textLenBeforePadding + padding + 1);
        if (temp_text == NULL) {
            perror("ERROR: Memory reallocation failed");
            free(text);
            free(binaryText);
            exit(EXIT_FAILURE);
        }
        binaryText = temp_text;
        for (int i = 0; i < padding; i++) {
            binaryText[textLenBeforePadding + i] = '0';
        }
        binaryText[textLenBeforePadding + padding] = '\0'; // Null-termino
    }

    return binaryText;
}

/**
 * Funzione per cifrare il testo binario tramite XOR con la chiave
 * @param buffer: il testo in binario da cifrare
 * @param threadCount: il numero di thread da utilizzare (grado di parallelismo)
 * @param key: la chiave di cifratura
 * @return: il testo in binario cifrato
 */
char *cipherBinary(char* buffer ,int threadCount, char *key) {

    int len =  (int)strlen(buffer);
    char* result = (char*)calloc(len+1, sizeof(char)); // Alloca memoria per il testo cifrato
    int numBlocks = len / 64; // 176 / 64 = 2
    int numBlocksForThread = numBlocks / threadCount; // 3 / 2 = 1
    int remainingBlocks = numBlocks % threadCount; // 3 % 2 = 1
    int lastPointer = 0; // Ultima posizione assegnata a un thread

    // Se il numero di thread è maggiore del numero di blocchi, si aggiusta il numero di thread
    if (threadCount > numBlocks) {
        threadCount = numBlocks;
    }

    // Array di thread per gestire i thread creati
    pthread_t threads[threadCount];

    // Inizializza il mutex per la mutua esclusione tra i thread
    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("ERROR: Mutex init failed");
        free(result);
        free(buffer);
        free(key);
        exit(EXIT_FAILURE);
    }

    // Inizializza la barriera per sincronizzare i thread per bloccare fino a threadCount numero di threads
    if (pthread_barrier_init(&barrier, NULL, threadCount) != 0) {
        perror("Failed to initialize barrier");
        free(result);
        free(buffer);
        free(key);
        pthread_mutex_destroy(&lock);
        exit(EXIT_FAILURE);
    }


    for (int i = 0; i < threadCount; i++) {

        // Alloca memoria per gli argomenti del thread i-esimo
        thread_args *args = malloc(sizeof(thread_args));
        if (!args) {
            perror("ERROR: Failed to allocate thread arguments");
            free(result);
            free(buffer);
            free(key);
            free(args);
            pthread_barrier_destroy(&barrier);
            pthread_mutex_destroy(&lock);
            exit(EXIT_FAILURE);
        }

        // Se ci sono blocchi rimanenti, assegna un blocco in più a questo thread
        if (remainingBlocks>0) {
            args->numBlocks = numBlocksForThread + 1;
            remainingBlocks--;
        } else {
            args->numBlocks = numBlocksForThread;
        }

        // Calcola l'inizio relativo del testo binario per questo thread
        char *startP = &buffer[lastPointer]; // Sarebbe buffer + lastPointer
        lastPointer = startP - buffer;

        args->id = i;
        args->key = key;
        args->result = result; // Risorsa condivisa tra tutti i thread, dove il risultato sarà scritto
        args->startP = buffer+lastPointer; // Inizio relativo del testo binario sulla quale operare per questo thread
        args->resultStart = lastPointer; // Inizio relativo del risultato sulla quale operare per questo thread

        lastPointer += (args->numBlocks * 64); // Aggiorno lastPointer per il prossimo thread


        // Creo il thread i-esimo e passo la funzione da eseguire e gli argomenti
        if (pthread_create(&threads[i], NULL, execXOR, args) != 0) {
            perror("ERROR: Failed to create thread");
            free(args);
            free(result);
            free(buffer);
            free(key);
            pthread_barrier_destroy(&barrier);
            pthread_mutex_destroy(&lock);
            exit(EXIT_FAILURE);
        }
    }

    /*
     * Il processo padre si blocca fino a quando tutti i thread non hanno raggiunto la barriera nel execXOR
     * Quando tutti i thread hanno raggiunto la barriera, i thread si sbloccano
     * e il pthread_join fa sbloccare il processo padre
     */
    for (int i = 0; i < threadCount; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("ERROR: Failed to join thread");
            free(result);
            free(buffer);
            free(key);
            pthread_barrier_destroy(&barrier);
            pthread_mutex_destroy(&lock);
            exit(EXIT_FAILURE);
        }
    }

    // Distruggo la barriera e il mutex dopo l'uso
    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&lock);

    if (debug==1) {
        printf("CLIENT: All threads have finished\n");
        printf("CLIENT: Ciphered text (binary length %d):\n%s\n", len, result);
    }

    return result;
}

/**
 * Funzione eseguita da ogni thread per eseguire l'operazione XOR tra il testo binario e la chiave
 * @param args: puntatore alla struttura thread_args contenente gli argomenti del thread i-esimo
 */
void* execXOR(void* args) {
    thread_args *t_args = (thread_args *)args; // Cast del puntatore args a thread_args
    char* key = t_args->key;
    char* startP = t_args->startP;

    /*
     * Utilizzo del mutex per garantire la mutua esclusione alla risorsa
     */
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
                result[resultStart+i*64+j] = '1';
            }
            pthread_mutex_unlock(&lock);
        }
    }

    // Libero la memoria allocata per gli argomenti del thread
    free(t_args);

    // Sincronizzo i thread con la barriera insieme al processo padre
    int s = pthread_barrier_wait(&barrier);  // All threads block here until NUM_THREADS arrive
    if (s!= 0 && s != PTHREAD_BARRIER_SERIAL_THREAD) {
        perror("Barrier wait failed");
        exit(EXIT_FAILURE);
    }
}

/**
 * Funzione per convertire una stringa in binario
 * @param string: la stringa da convertire
 * @return: la rappresentazione in binario della stringa
 */
char* convertToBinary(char* string) {

    int len = (int)strlen(string); // Lunghezza della stringa normale
    int bitsLen = len * 8; // Lunghezza in bit del risultato
    char* binary = (char*)malloc(bitsLen+1); // +1 per il '\0'

    // Convert each of the first 8 bytes to binary
    for (int i = 0; i < len; i++) {

        unsigned char c = string[i]; // I valori unsigned char vanno da 0 a 255, cioè 8 bit

        for (int j = 0; j < 8; j++) {
            // Si usa l'operatore AND bit a bit per convertire il carattere in binario tramite bit shifting
            binary[i * 8 + j] = (c & (1 << (7 - j))) ? '1' : '0';
        }
    }
    binary[bitsLen] = '\0'; // Null-termino la stringa in binario

    return binary;
}

void init_client(char* serverAddr, int portno, char* binaryText, char* key) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct in_addr addr;

    int binaryTextLen = (int)strlen(binaryText); // Lunghezza del testo binario
    char binaryTextLenStr[20];
    bzero(binaryTextLenStr, sizeof(binaryTextLenStr)); // Per pulire il buffer
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
        exit(EXIT_FAILURE);
    }

    int n = 0;
    n = write(sockfd, binaryTextLenStr, sizeof(binaryTextLenStr));
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(EXIT_FAILURE);
    }
    n = write(sockfd, binaryText, strlen(binaryText));
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(EXIT_FAILURE);
    }
    n = write(sockfd, key, 64);
    if (n < 0) {
        perror("ERROR writing to socket");
        exit(EXIT_FAILURE);
    }

    // Legge la risposta dal server
    char buffer[256];
    bzero(buffer,256);
    n = read(sockfd,buffer,255);
    if (n < 0) {
        perror("ERROR reading from socket");
        exit(EXIT_FAILURE);
    }
    printf("%s",buffer);
    close(sockfd);
}



// Modo alternativo per ottenere l'indirizzo IP del server
// if (inet_pton(AF_INET, serverAddr, &serv_addr.sin_addr) <= 0) {
//     server = gethostbyname(serverAddr);
//     bcopy((char *)server->h_addr_list[0], (char *)&serv_addr.sin_addr.s_addr, server->h_length);
// }

// Esempio di input dall'utente
// char buffer[256];
// bzero(buffer,256); // Sets 256 bytes to 0 in buffer like a calloc
// printf("Please enter the message: ");
// fgets(buffer,255,stdin);
