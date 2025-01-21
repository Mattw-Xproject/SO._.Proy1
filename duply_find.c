#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#define MAX_FILES 1024
#define MAX_PATH 1024

typedef struct {
    char path[MAX_PATH];
} FileNode;

typedef struct {
    FileNode files[MAX_FILES];
    int count;
} FileList;

FileList to_visit;
FileList visited;
sem_t mutex;
sem_t sem_to_visit;
sem_t sem_visited;

void *check_duplicates(void *arg);
void add_to_visit(const char *path);
void add_to_visited(const char *path);
int is_duplicate(const char *file1, const char *file2);
void process_directory(const char *dir_path);

int main(int argc, char *argv[]) {
    if (argc != 5 || strcmp(argv[1], "-t") != 0 || strcmp(argv[3], "-d") != 0) {
        fprintf(stderr, "Uso: %s -t <numero de threads> -d <directorio de inicio>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int num_threads = atoi(argv[2]);
    const char *start_dir = argv[4];

    // Inicializar listas y semáforos
    to_visit.count = 0;
    visited.count = 0;
    sem_init(&mutex, 0, 1);
    sem_init(&sem_to_visit, 0, 0);
    sem_init(&sem_visited, 0, 1);

    // Agregar el directorio inicial a la lista de archivos a visitar
    add_to_visit(start_dir);

    // Crear hilos
    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, check_duplicates, NULL);
    }

    // Esperar a que los hilos terminen
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    // Limpiar semáforos
    sem_destroy(&mutex);
    sem_destroy(&sem_to_visit);
    sem_destroy(&sem_visited);

    return EXIT_SUCCESS;
}

void *check_duplicates(void *arg) {
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

        // Procesar el directorio o archivo
        struct stat statbuf;
        if (stat(current_file, &statbuf) == -1) {
            perror("stat");
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            process_directory(current_file);
        } else if (S_ISREG(statbuf.st_mode) && statbuf.st_size > 0) {
            // Comparar con archivos visitados
            sem_wait(&sem_visited);
            for (int i = 0; i < visited.count; i++) {
                if (is_duplicate(current_file, visited.files[i].path)) {
                    printf("Duplicado encontrado: %s y %s\n", current_file, visited.files[i].path);
                }
            }
            add_to_visited(current_file);
            sem_post(&sem_visited);
        }
    }
    return NULL;
}

void add_to_visit(const char *path) {
    sem_wait(&mutex);
    if (to_visit.count < MAX_FILES) {
        strcpy(to_visit.files[to_visit.count++].path, path);
        sem_post(&sem_to_visit);
    }
    sem_post(&mutex);
}

void add_to_visited(const char *path) {
    sem_wait(&mutex);
    if (visited.count < MAX_FILES) {
        strcpy(visited.files[visited.count++].path, path);
    }
    sem_post(&mutex);
}

int is_duplicate(const char *file1, const char *file2) {
    // Aquí se puede implementar una comparación más robusta, como comparar hashes
    return strcmp(file1, file2) == 0; // Comparación simple por nombre
}

void process_directory(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
            if (entry->d_type == DT_DIR) {
                add_to_visit(full_path); // Agregar directorio a visitar
            } else if (entry->d_type == DT_REG) {
                add_to_visit(full_path); // Agregar archivo regular a visitar
            }
        }
    }
    closedir(dir);
} 