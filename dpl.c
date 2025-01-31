#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_FILES 4096
#define MAX_PATH 1024
#define HASH_SIZE 33 // 32 caracteres + 1 para el terminador nulo

typedef struct {
    char path[MAX_PATH];
} FileNode;

typedef struct {
    FileNode files[MAX_FILES];
    int count;
} FileList;


typedef struct {
    char file1[MAX_PATH];
    char file2[MAX_PATH];
} DuplicatePair;

FileList to_visit;
FileList visited;
DuplicatePair duplicates[MAX_FILES]; // Para almacenar pares de duplicados
int duplicate_count = 0; // Contador de duplicados

sem_t mutex;
sem_t sem_to_visit;
sem_t sem_visited;
pthread_cond_t cond_to_visit;

void *check_duplicates(void *arg);
void add_to_visit(const char *path);
void add_to_visited(const char *path);
int is_duplicate(const char *file1, const char *file2, char mode); // Modificado para incluir el modo
int get_md5_hash_executable(const char *filename, char *hash_output);
int get_md5_hash_library(const char *filename, char *hash_output); // Nueva función para la biblioteca
void process_directory(const char *dir_path);

int main(int argc, char *argv[]) {
    if (argc != 7 || strcmp(argv[1], "-t") != 0 || strcmp(argv[3], "-d") != 0 || strcmp(argv[5], "-m") != 0) {
        fprintf(stderr, "Uso: %s -t <numero de threads> -d <directorio de inicio> -m <e | l>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int num_threads = atoi(argv[2]);
    const char *start_dir = argv[4];
    char mode = argv[6][0]; // 'e' o 'l'
	
	duplicate_count = 0; // Reiniciar contador de duplicados
    // Inicializar listas y semáforos
    to_visit.count = 0;
    visited.count = 0;
    sem_init(&mutex, 0, 1);
    sem_init(&sem_to_visit, 0, 0);
    sem_init(&sem_visited, 0, 1);
    pthread_cond_init(&cond_to_visit, NULL);

    // Agregar el directorio inicial a la lista de archivos a visitar
    add_to_visit(start_dir);

    // Crear hilos
    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, check_duplicates, (void *)&mode);
    }

    // Esperar a que los hilos terminen
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Imprimir estadísticas de duplicados

    printf("Se han encontrado %d archivos duplicados.\n", duplicate_count);

    for (int i = 0; i < duplicate_count; i++) {

        printf("%s es duplicado de %s\n", duplicates[i].file1, duplicates[i].file2);

    }

    // Limpiar semáforos
    sem_destroy(&mutex);
    sem_destroy(&sem_to_visit);
    sem_destroy(&sem_visited);
    pthread_cond_destroy(&cond_to_visit);

    return EXIT_SUCCESS;
}

void *check_duplicates(void *arg) {
    char mode = *(char *)arg; // Obtener el modo de hash

    while (1) {
        // Esperar a que haya archivos a visitar
        sem_wait(&sem_to_visit);

        // Bloquear acceso a la lista de archivos a visitar
        sem_wait(&mutex);
        if (to_visit.count == 0) {
            sem_post(&mutex);
            break; // Salir si no hay más archivos a visitar
        }
        // Obtener el siguiente archivo a visitar
        char current_file[MAX_PATH];
        strcpy(current_file, to_visit.files[--to_visit.count].path);
        sem_post(&mutex);


        // Verificar si el archivo es un directorio
        struct stat statbuf;
        if (stat(current_file, &statbuf) == -1) {
            perror("stat");
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            // Procesar el directorio
            DIR *dir = opendir(current_file);
            if (dir == NULL) {
                perror("opendir");
                continue;
            }

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                    char full_path[MAX_PATH];
                    snprintf(full_path, sizeof(full_path), "%s/%s", current_file, entry->d_name);
                    add_to_visit(full_path); // Agregar archivos encontrados a la lista
                }
            }
            closedir(dir);
        } else if (S_ISREG(statbuf.st_mode) && statbuf.st_size > 0) {
            // Comparar con archivos visitados
            sem_wait(&sem_visited);
            for (int i = 0; i < visited.count; i++) {
                if (is_duplicate(current_file, visited.files[i].path, mode)) { // Usar el modo
                    // Almacenar el par de duplicados

                    strcpy(duplicates[duplicate_count].file1, current_file);

                    strcpy(duplicates[duplicate_count].file2, visited.files[i].path);

                    duplicate_count++;
                }
            }
            add_to_visited(current_file);
            sem_post(&sem_visited);
        }

        // Verificar si no hay más archivos a visitar
        sem_wait(&mutex);
        if (to_visit.count == 0) {
            pthread_cond_broadcast(&cond_to_visit); // Notificar a otros hilos
            sem_post(&mutex);
            break; // Salir si no hay más archivos a visitar
        }
        sem_post(&mutex);
    }
    return NULL;
}

void add_to_visit(const char *path) {
    sem_wait(&mutex);
    strcpy(to_visit.files[to_visit.count++].path, path);
    sem_post(&mutex);
    sem_post(&sem_to_visit);
}

void add_to_visited(const char *path) {
    sem_wait(&mutex);
    strcpy(visited.files[visited.count++].path, path);
    sem_post(&mutex);
}

int get_md5_hash_executable(const char *filename, char *hash_output) {
    int pipefd[2];
    pid_t pid;
    char command[256];

    // Crear la tubería
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }

    // Crear un nuevo proceso
    pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) { // Proceso hijo
        // Cerrar la lectura de la tubería
        close(pipefd[0]);

        // Redirigir la salida estándar a la tubería
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        // Ejecutar el comando
        execlp("./md5", "./md5", filename, (char *)NULL);

        // Si execlp falla
        perror("execlp");
        exit(EXIT_FAILURE);
    } else { // Proceso padre
        // Cerrar la escritura de la tubería
        close(pipefd[1]);

        // Leer el hash de la tubería
        read(pipefd[0], hash_output, HASH_SIZE);
        hash_output[HASH_SIZE - 1] = '\0'; // Asegurarse de que la cadena esté terminada

        // Esperar a que el proceso hijo termine
        wait(NULL);
    }

    // Cerrar la lectura de la tubería
    close(pipefd[0]);
    return 0;
}

int get_md5_hash_library(const char *filename, char *hash_output) {
    return MDFile((char *)filename, hash_output);
}

int is_duplicate(const char *file1, const char *file2, char mode) {
    char hash1[HASH_SIZE];
    char hash2[HASH_SIZE];

    if (mode == 'e') {
        if (get_md5_hash_executable(file1, hash1) == -1 || get_md5_hash_executable(file2, hash2) == -1) {
            return 0; // Error al obtener el hash
        }
    } else if (mode == 'l') {
        if (get_md5_hash_library(file1, hash1) == 0 || get_md5_hash_library(file2, hash2) == 0) {
            return 0; // Error al obtener el hash
        }
    } else {
        return 0; // Modo no válido
    }

    return strcmp(hash1, hash2) == 0 ? 1 : 0; // Retorna 1 si son duplicados, 0 si no
}

void process_directory(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    struct dirent *entry;

    if (dir == NULL) {
        perror("No se puede abrir el directorio");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { // Solo archivos regulares
            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
            add_to_visited(full_path);
        }
    }
    closedir(dir);
}
