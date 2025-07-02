#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include <errno.h>


void init_server(int max_threads, char* prefix, int connection_num);
void readfile(int sockfd, int thread_num, char *string);
void* execXOR(void* args);
char* processBinaryText(char* binaryText, char* key, int threadCount);

/**
 * Variabile globale per il debug, impostata a 1 per avere sullo stdout le informazioni di debug
 */
const int debug = 1;

/**
 * Struttura per gestire i nodi della lista concatenata
 */
typedef struct BlockNode {
    char block[65];
    struct BlockNode *next; // Puntatore al successivo blocco della lista concatenata
} BlockNode;

/**
 * Struttura per passare gli argomenti ai thread
 */
typedef struct {
    int id;
    char* key;
    char* startP;
    BlockNode* result;
    int numBlocks;
} thread_args;

/**
 * Mutex per la sincronizzazione tra i thread
 */
pthread_mutex_t lock;

/**
 * Funzione main del server
 */
int main(int argc, char *argv[]) {

    int threads, max_connections;
    char* prefix;
    int option;

    while ((option = getopt(argc, argv, "t:p:c:")) != -1) { // t: threads, p: prefix, c: max connections
        switch (option) {
            case 't':
                errno = 0; // strtol imposta errno a 0 se non ci sono errori
                threads = (int)strtol(optarg, NULL, 10); // Casting in quanto strtol restituisce long int
                if (errno == ERANGE) {
                    perror("strtol");
                    exit(EXIT_FAILURE); // Stessa cosa di exit(1);
                }
                break;
            case 'p':
                prefix = optarg;
                break;
            case 'c':
                // max_connections = atoi(optarg); // atoi non gestisce gli errori -> strtol
                errno = 0;
                max_connections = (int)strtol(optarg, NULL, 10); // Casting in quanto strtol restituisce long int
                if (errno == ERANGE) {
                    perror("strtol");
                    exit(EXIT_FAILURE);
                }
                break;
            default:
                perror("missing arguments");
        }
    }

    init_server(threads, prefix, max_connections);
}

/**
 * Funzione per inizializzare il server
 * @param max_threads: grado di parallelismo che il server deve utilizzare
 * @param prefix: prefisso per i file di output
 * @param connection_num: numero massimo di connessioni simultanee possibili
 */
void init_server(int max_threads, char* prefix, int connection_num) {

    int sockfd, newsockfd, pid; // Identificatori dei socket
    int portno = 5431; // Porta di ascolto del server
    socklen_t clilen; // Lunghezza della struttura sockaddr_in per il client
    struct sockaddr_in serv_addr, cli_addr; // Strutture per gli indirizzi del server e del client

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(EXIT_FAILURE);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr)); // Pulisce la struttura serv_addr
    serv_addr.sin_family = AF_INET; // Indica che si sta utilizzando IPv4
    serv_addr.sin_port = htons(portno); // Imposta la porta del server
    serv_addr.sin_addr.s_addr = INADDR_ANY; // Accetta connessioni da qualsiasi indirizzo IP

    // Associa il socket all'indirizzo e alla porta specificati
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == -1) {
        perror("ERROR on bind");
        exit(EXIT_FAILURE);
    }

    // Imposta il socket in ascolto per le connessioni in arrivo
    if (listen(sockfd, connection_num) == -1) {
        perror("ERROR on listen");
        exit(EXIT_FAILURE);
    }

    if (debug){
        printf("SERVER: Listening on port %d...\n", portno);
    }

    clilen = sizeof(cli_addr); // Inizializza la lunghezza della struttura cli_addr

    // Ascolta continuamente per nuove connessioni
    while (1) {

        if (connection_num > 0) {

            newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);

            if (newsockfd == -1) {
                perror("ERROR on accept");
                exit(EXIT_FAILURE);
            }

            connection_num--;

            if (debug) {
                printf("SERVER: Connection accepted, remaining connections: %d\n", connection_num);
            }

            pid = fork();
            if (pid == -1) {
                perror("ERROR on fork");
                exit(EXIT_FAILURE);
            }

            if (pid == 0) { // fork restituisce 0 al processo figlio
                close(sockfd); // Chiude il socket del server nel processo figlio
                if (debug) {
                    printf("SERVER (Child %d): Handling connection\n", getpid());
                }
                readfile(newsockfd, max_threads, prefix); // Legge il file dal client e lo processa
                exit(EXIT_SUCCESS); // Termina il processo figlio dopo aver gestito la connessione
            }
            else {
                close(newsockfd); // Chiude il socket del client nel processo padre
                waitpid(pid, NULL, WNOHANG); // Attende che il processo figlio termini senza bloccare
                connection_num++;
            }
        }
    }
}

/**
 * Funzione per convertire un testo binario in una stringa
 * @param text: il testo da convertire
 * @return: la stringa binaria corrispondente al testo
 */
char* convertToString(char* text) {

    if (text == NULL) {
        fprintf(stderr, "ERROR: text to convert is NULL\n");
        exit(EXIT_FAILURE);
    }

    int binaryLen = (int)strlen(text); // Casting da long a int
    int charCount = binaryLen / 8; // Ogni carattere è rappresentato da 8 bit

    char* result = malloc(charCount + 1); // +1 for null terminator
    if (result == NULL) {
        perror("ERROR on memory allocation");
        free(text);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < charCount; i++) {
        uint8_t byte = 0; // Intero a 8 bit senza segno, quindi da 0 a 255
        char* byteStart = text + (i * 8); // Ottieni l'inizio dei 8 bit per il carattere i-esimo

        // Converte 8 bit binari in un byte
        for (int bit = 0; bit < 8; bit++) { // Si considerano solo i primi 8 bit, ovvero quelli del carattere i-esimo
            if (byteStart[bit] == '1') { // Se il bit è 1 allora prima lo inserisco alla posizione corretta usando lo shift
                byte |= (1 << (7 - bit)); // OR bit a bit con il byte corrente (e.g. ...010 | ...001 = ...011)
            }
            else if (byteStart[bit] != '0') { // Se il bit non è né 0 né 1, allora è un errore
                fprintf(stderr, "ERROR: Invalid binary digit '%c'\n", byteStart[bit]);
                free(result);
                free(text);
                exit(EXIT_FAILURE);
            }
        }
        result[i] = (char)byte; // Dopo aver convertito i 8 bit in un byte, lo inserisco nella stringa di output
    }
    result[charCount] = '\0'; // Null-termino
    return result;
}

/**
 * Funzione per scrivere il risultato in un file
 * @param resultText: il testo ottenuto e tradotto da scrivere nel file
 * @param string: il prefisso del file
 */
void writeFile(char* resultText, char* string) {

    if (resultText == NULL || string == NULL) {
        fprintf(stderr, "ERROR: resultText or string is NULL\n");
        exit(EXIT_FAILURE);
    }

    // Crea il file di output con il prefisso e l'estensione .txt
    char outputFileName[256];
    snprintf(outputFileName, sizeof(outputFileName), "%s.txt", string);

    FILE *outputFile = fopen(outputFileName, "w");
    if (outputFile == NULL) {
        perror("ERROR: Failed to open output file");
        free(resultText);
        free(string);
        exit(EXIT_FAILURE);
    }

    fprintf(outputFile, "%s", resultText); // Scrive il testo nel file di output
    fclose(outputFile);
    free(resultText);
    free(string);

    if (debug==1) {
        printf("SERVER: Result written to %s\n", outputFileName);
    }
}

/**
 * Funzione per leggere il file dal socket e processarlo
 * @param sockfd: il socket da cui leggere i dati
 * @param thread_num: il numero di thread da utilizzare per il processamento
 * @param string: il prefisso del file di output
 */
void readfile(int sockfd, int thread_num, char *string) {

    if (sockfd < 0 || string == NULL) {
        fprintf(stderr, "ERROR: Invalid socket or string\n");
        free(string);
        exit(EXIT_FAILURE);
    }

    int read_num;
    char* prefix = string;

    // Si legge la lunghezza del testo binario dal socket
    char binaryTextLenStr[20];
    bzero(binaryTextLenStr,20);
    read_num = (int)read(sockfd, binaryTextLenStr, sizeof(binaryTextLenStr));
    if (read_num == -1) {
        perror("ERROR reading from socket");
        free(string);
        exit(EXIT_FAILURE);
    }
    errno = 0; // Resetta errno prima di usare strtol
    int binaryTextLen = (int)strtol(binaryTextLenStr, NULL, 10);
    if (errno == ERANGE) {
        perror("strtol");
        free(string);
        exit(EXIT_FAILURE);
    }


    // Si legge il testo binario dal socket
    char binaryText[binaryTextLen]; // Lunghezza ottenuta prima
    read_num = (int)read(sockfd, binaryText, sizeof(binaryText));
    if (read_num == -1) {
        perror("ERROR reading from socket");
        free(string);
        exit(EXIT_FAILURE);
    }
    binaryText[read_num] = '\0'; // Null-termino

    // Si legge la chiave di cifratura dal socket
    char key[64];
    read_num = (int)read(sockfd, key, sizeof(key));
    if (read_num == -1) {
        perror("ERROR reading from socket");
        free(string);
        exit(EXIT_FAILURE);
    }
    if (read_num != 64) {
        fprintf(stderr, "ERROR: Key must be exactly 64 characters long\n");
        free(string);
        exit(EXIT_FAILURE);
    }
    key[read_num] = '\0'; // Null-termino

    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): The binaryTextLen is: %s\n", pid, binaryTextLenStr);
        printf("SERVER (Child %d): The binaryText is: %s\n", pid, binaryText);
        printf("SERVER (Child %d): The key is: %s\n", pid, key);
    }

    // Processo il testo binario con la chiave
    char* resultText = processBinaryText(binaryText, key, thread_num);
    resultText = convertToString(resultText); // Converte il testo binario processato in stringa

    // Manda una risposta al client se arrivato a questo punto correttamente
    read_num = (int)write(sockfd, "SERVER: Correctly received ciphered text\n", 41); // 41 è la lunghezza della stringa da mandare
    if (read_num == -1) {
        perror("ERROR writing to socket");
        free(string);
        free(resultText);
        exit(EXIT_FAILURE);
    }
    fflush(stdout); // Forza la scrittura su stdout
    usleep(100000); // Attende 100 millisecondi per assicurarsi che il client riceva il messaggio
    close(sockfd); // Chiude il socket dopo aver inviato la risposta
    // Scrive il risultato in un file
    writeFile(resultText, prefix);
}

/**
 * Funzione per processare il testo binario con la chiave
 * @param binaryText: il testo binario da processare
 * @param key: la chiave di cifratura
 * @param threadCount: il numero di thread da utilizzare
 * @return: il testo binario processato
 */
char* processBinaryText(char* binaryText, char* key, int threadCount) {

    int binaryTextLen = (int)strlen(binaryText);
    int numTotalBlocks = binaryTextLen / 64;

    if (binaryTextLen % 64 != 0) {
        fprintf(stderr, "ERROR: Binary text length is not a multiple of 64\n");
        free(binaryText);
        exit(EXIT_FAILURE);
    }

    // Alloca memoria per gestire i thread e i loro argomenti
    pthread_t *threads = malloc(threadCount * sizeof(pthread_t)); // Array di identificatori dei thread
    thread_args **args_array = malloc(threadCount * sizeof(thread_args*)); // Array di puntatori a strutture thread_args che contengono gli argomenti per ogni thread
    if (!threads || !args_array) {
        perror("SERVER: Failed to allocate memory for thread management structures");
        if(threads) free(threads); // Se threads è stato allocato, lo libero
        if(args_array) free(args_array); // Se args_array è stato allocato, lo libero
        exit(EXIT_FAILURE);
    }

    // Inizializza il mutex per la sincronizzazione tra i thread
    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("ERROR: Mutex init failed");
        free(threads);
        free(args_array);
        exit(EXIT_FAILURE);
    }

    // Inizializzazione della lista concatenata per il risultato
    BlockNode* result_list_head = NULL;
    BlockNode* result_list_tail = NULL;

    int blocksPerThread_base = numTotalBlocks / threadCount; // Numero di blocchi base per thread
    int remainingBlocks_distribute = numTotalBlocks % threadCount; // Numero di blocchi rimanenti da distribuire tra i thread

    int currentInputOffset = 0; // Questo offset viene utilizzato per calcolare l'inizio relativo del testo binario per ogni thread

    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): Threads: %d, Total blocks: %d, Default blocks per thread: %d, Remaining blocks to distribute: %d\n", pid, threadCount, numTotalBlocks, blocksPerThread_base, remainingBlocks_distribute);
    }

    for (int i = 0; i < threadCount; i++) {

        args_array[i] = malloc(sizeof(thread_args)); // Alloca memoria per la struttura thread_args per il thread i-esimo
        if (!args_array[i]) {
            perror("SERVER: Malloc failed for thread_args");
            exit(EXIT_FAILURE);
        }

        thread_args *current_args = args_array[i]; // Puntatore alla struttura thread_args per il thread i-esimo
        // Assegno i valori agli argomenti del thread
        current_args->id = i; // ID del thread
        current_args->key = key; // Chiave di cifratura comune
        current_args->numBlocks = blocksPerThread_base; // Numero di blocchi assegnati a questo thread
        if (remainingBlocks_distribute > 0) { // Se ci sono blocchi rimanenti da distribuire, assegno uno in più a questo thread
            current_args->numBlocks++;
            remainingBlocks_distribute--;
        }
        current_args->startP = binaryText + currentInputOffset; // Puntatore all'inizio relativo del testo binario per questo thread
        currentInputOffset += (current_args->numBlocks * 64); // Aggiorno l'offset per il prossimo thread

        BlockNode* localFirstNode = NULL; // Primo nodo della lista concatenata locale per questo thread
        BlockNode* localCurrentNewNode = NULL; // Nodo corrente della lista concatenata locale per questo thread

        /*
         * Questo for serve per allocare i nodi della lista concatenata per il thread i-esimo.
         */
        for (int j = 0; j < current_args->numBlocks; j++) {
            BlockNode *newNode = (BlockNode *)malloc(sizeof(BlockNode));
            if (!newNode) {
                perror("Failed to allocate memory for block node");
                free(current_args);
                exit(EXIT_FAILURE);
            }

            memset(newNode->block, 0, 64); // Inizializza il blocco del nodo a zero
            newNode->block[64] = '\0';      // Null-termino
            newNode->next = NULL; // Inizializza il puntatore al nodo successivo a NULL

            if (localFirstNode == NULL) { // Se la testa della lista locale è NULL, significa che questo è il primo nodo
                localFirstNode = newNode;
            }
            if (localCurrentNewNode != NULL) { // Se esiste un nodo corrente, lo collego al nuovo nodo
                localCurrentNewNode->next = newNode;
            }
            localCurrentNewNode = newNode; // Aggiorno il nodo corrente per il prossimo ciclo


            // Appendo il nuovo nodo alla lista concatenata globale
            // Se la testa della lista globale è NULL, significa che questo è il primo nodo
            if (result_list_head == NULL) {
                result_list_head = newNode;
                result_list_tail = newNode; // Dato che il primo blocco è anche l'ultimo all'inizio
            } else { // Altrimenti, lo aggiungo alla fine della lista globale
                result_list_tail->next = newNode;
                result_list_tail = newNode;
            }
        }
        // Assegno al thread i-esimo la testa del nodo dalla quale inizierà a scrivere i blocchi processati
        current_args->result = localFirstNode; // Head of this thread's output list

        if (debug==1) {
            int pid = getpid();
            printf("SERVER (Child %d): Thread %d: numBlocks=%d, input_offset=%ld\n", pid, i, current_args->numBlocks, (current_args->startP - binaryText));
        }

        // Creo il thread i-esimo e passo la funzione da eseguire e gli argomenti
        if (pthread_create(&threads[i], NULL, execXOR, current_args) != 0) {
            perror("ERROR: Failed to create thread");
            free(current_args);

            // Libero la memoria allocata per i nodi della lista concatenata
            BlockNode* currentBlock = result_list_head;
            while (currentBlock != NULL) {
                BlockNode* temp = currentBlock;
                currentBlock = currentBlock->next;
                free(temp);
            }
            free(threads);
            free(args_array);
            exit(EXIT_FAILURE);
        }
    }

    // Inserisco i thread nella barriera per sincronizzare l'esecuzione
    for (int i = 0; i < threadCount; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("ERROR: Failed to join thread");
            exit(EXIT_FAILURE);
        }
    }

    // Alloca memoria per il buffer finale che conterrà il risultato concatenato
    char* finalResultBuffer = (char*)malloc(numTotalBlocks * 64 + 1);
    if (!finalResultBuffer) {
        perror("ERROR: malloc for final_result_buffer failed");
        exit(EXIT_FAILURE);
    }
    finalResultBuffer[0] = '\0'; // Inizializzo il buffer finale a una stringa vuota per strcat

    // Unisco i blocchi della lista concatenata in finalResultBuffer
    BlockNode* currentBlockToConcat = result_list_head;
    while (currentBlockToConcat != NULL) {
        strncat(finalResultBuffer, currentBlockToConcat->block, 64); // Uso strncat per gestire blocchi da 64
        BlockNode* temp = currentBlockToConcat;
        currentBlockToConcat = currentBlockToConcat->next;
        free(temp); // Nel mentre libero il nodo non più necessario
    }

    if (debug==1) {
        int pid = getpid();
        printf("SERVER (Child %d): Resulting binary text: %s\n", pid, finalResultBuffer);
        fflush(stdout); // Forza la scrittura su stdout per questione di debug
    }

    // Distruggo il mutex
    pthread_mutex_destroy(&lock);

    // Libero la memoria allocata per i thread e le strutture associate (argomenti dei thread)
    free(threads);
    free(args_array);
    return finalResultBuffer;
}

/**
 * Funzione eseguita da ogni thread per eseguire l'operazione XOR tra il testo binario e la chiave
 * @param args: puntatore alla struttura thread_args contenente gli argomenti del thread i-esimo
 */
void* execXOR(void* args) {
    thread_args *t_args = (thread_args *)args;
    char* key = t_args->key; // Chiave di cifratura
    char* startP = t_args->startP; // Puntatore all'inizio relativo del testo binario per questo thread

    /*
     * Sincronizza l'accesso alla lista concatenata dei blocchi
     */
    pthread_mutex_lock(&lock);
    BlockNode* currentResultNode = t_args->result;
    pthread_mutex_unlock(&lock);

    int threadNum = t_args->numBlocks;
    int keyLen = (int)strlen(key);

    // Eseguo l'operazione XOR tra il testo binario e la chiave
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
        currentResultNode->block[64] = '\0'; // Null-termino il blocco per evitare problemi di concatenazione dopo con strncat
        currentResultNode = currentResultNode->next;
    }
    free(t_args);
}
