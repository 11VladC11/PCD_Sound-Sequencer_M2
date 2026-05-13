/*
* Cojocaru Vlad
 * IR3 - grupa 4
 * modul runtime_state
 * centralizeaza starea partajata a serverului (admin + remote + worker),
 * pentru a avea un singur punct de adevar in operatiile din milestone 2.
 */

#include "runtime_state.h" /* API public runtime, pentru implementarea starii partajate */

#include <errno.h> /* errno/EINTR, pentru robustete la operatii pe fisiere */
#include <fcntl.h> /* open flags, pentru fisierele .upload si .wav */
#include <pthread.h> /* mutex/cond/thread, pentru sincronizare si worker de coada */
#include <stdio.h> /* snprintf/strftime, pentru mesaje admin si formatare text */
#include <string.h> /* strcmp, pentru potrivirea jobului curent */
#include <sys/stat.h> /* permisiuni fisier, pentru creare output cu mod controlat */
#include <sys/types.h> /* tipuri POSIX, pentru compatibilitate API sistem */
#include <time.h> /* time/localtime_r, pentru timestamp in history/stats */
#include <unistd.h> /* read/write/close, pentru copierea fisierelor si cleanup */

enum {
    /* stari job, pentru raportare STATUS catre clienti */
    status_idle = 0,
    status_uploading = 1,
    status_queued = 2,
    status_processing = 3,
    status_ready = 4,
    status_error = 5,
    io_buffer_size = 65536,
    file_permissions = 0644,
    queue_capacity = 32 /* coada simpla FIFO, pentru milestone curent */
};

/* stare globala protejata cu mutex, pentru partajare intre firele unix/inet/worker */
struct runtime_state {
    pthread_mutex_t lock;
    int is_initialized;

    char output_dir[marimecale];
    char current_name[marimetext];
    char upload_path[marimecale];
    char result_path[marimecale];
    size_t expected_bytes;
    size_t received_bytes;
    size_t result_bytes;
    size_t next_chunk_index;
    int status;
    int upload_fd;

    unsigned long total_remote_clients;
    unsigned long active_remote_clients;
    unsigned long total_admin_clients;
    unsigned long active_admin_clients;
    unsigned long remote_requests;
    unsigned long admin_requests;
    unsigned long jobs_completed;
    unsigned long jobs_failed;
    size_t total_bytes_in;
    size_t total_bytes_out;
    time_t last_update;
    int shutdown_requested;

    pthread_t worker_thread;
    pthread_cond_t queue_cond;
    int worker_started;
    char queue_names[queue_capacity][marimetext];
    char queue_upload_paths[queue_capacity][marimecale];
    char queue_result_paths[queue_capacity][marimecale];
    int queue_head;
    int queue_tail;
    int queue_count;
};

/* instanta unica a runtime-ului, pentru acces global controlat prin mutex */
static struct runtime_state g_runtime = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .is_initialized = 0,
    .status = status_idle,
    .upload_fd = -1
};

/* copiere sigura, pentru a valida lungimea stringurilor in buffere fixe */
static int safe_copy(char *dst, size_t dst_size, const char *src)
{
    int written;

    written = snprintf(dst, dst_size, "%s", src);
    if (written < 0 || (size_t)written >= dst_size) {
        return -1;
    }

    return 0;
}

/* construieste o cale de fisier output/name+suffix, pentru upload/result */
static int build_path(char *out, size_t out_size, const char *dir, const char *name, const char *suffix)
{
    int written;

    written = snprintf(out, out_size, "%s/%s%s", dir, name, suffix);
    if (written < 0 || (size_t)written >= out_size) {
        return -1;
    }

    return 0;
}

/* scrie tot payload-ul in descriptor, pentru a evita scriere partiala */
static int write_all(int fd, const unsigned char *data, size_t size)
{
    size_t written_total;

    written_total = 0U;
    while (written_total < size) {
        ssize_t current;

        current = write(fd, data + written_total, size - written_total);
        if (current < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        written_total += (size_t)current;
    }

    return 0;
}

/* copie stream din source in destination, pentru procesarea minima a jobului */
static int copy_file(const char *source, const char *destination, size_t *copied_bytes)
{
    unsigned char buffer[io_buffer_size];
    int source_fd;
    int destination_fd;
    size_t total;

    source_fd = open(source, O_RDONLY);
    if (source_fd < 0) {
        return -1;
    }

    destination_fd = open(destination, O_CREAT | O_WRONLY | O_TRUNC, file_permissions);
    if (destination_fd < 0) {
        (void)close(source_fd);
        return -1;
    }

    total = 0U;
    while (1) {
        ssize_t current;

        current = read(source_fd, buffer, sizeof(buffer));
        if (current < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(source_fd);
            (void)close(destination_fd);
            return -1;
        }

        if (current == 0) {
            break;
        }

        if (write_all(destination_fd, buffer, (size_t)current) != 0) {
            (void)close(source_fd);
            (void)close(destination_fd);
            return -1;
        }

        total += (size_t)current;
    }

    if (close(source_fd) < 0 || close(destination_fd) < 0) {
        return -1;
    }

    *copied_bytes = total;
    return 0;
}

/* converteste cod intern de status in text protocol */
static const char *status_to_text(int status)
{
    if (status == status_uploading) {
        return "UPLOADING";
    }
    if (status == status_queued) {
        return "QUEUED";
    }
    if (status == status_processing) {
        return "PROCESSING";
    }
    if (status == status_ready) {
        return "READY";
    }
    if (status == status_error) {
        return "ERROR";
    }
    return "IDLE";
}

/* worker-ul de coada, pentru procesare asincrona a upload-urilor finalizate */
static void *runtime_worker_main(void *unused)
{
    (void)unused;
    while (1) {
        char job_name[marimetext];
        char job_upload[marimecale];
        char job_result[marimecale];
        size_t copied_bytes;
        int pop_index;
        int copy_ok;

        if (pthread_mutex_lock(&g_runtime.lock) != 0) {
            return NULL;
        }

        while (g_runtime.queue_count == 0 && g_runtime.shutdown_requested == 0) {
            /* asteapta element nou in coada, pentru a nu face busy-wait */
            if (pthread_cond_wait(&g_runtime.queue_cond, &g_runtime.lock) != 0) {
                (void)pthread_mutex_unlock(&g_runtime.lock);
                return NULL;
            }
        }

        if (g_runtime.queue_count == 0 && g_runtime.shutdown_requested != 0) {
            (void)pthread_mutex_unlock(&g_runtime.lock);
            break;
        }

        pop_index = g_runtime.queue_head;
        g_runtime.queue_head = (g_runtime.queue_head + 1) % queue_capacity;
        g_runtime.queue_count--;
        if (safe_copy(job_name, sizeof(job_name), g_runtime.queue_names[pop_index]) != 0 ||
            safe_copy(job_upload, sizeof(job_upload), g_runtime.queue_upload_paths[pop_index]) != 0 ||
            safe_copy(job_result, sizeof(job_result), g_runtime.queue_result_paths[pop_index]) != 0) {
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            g_runtime.last_update = time(NULL);
            (void)pthread_mutex_unlock(&g_runtime.lock);
            continue;
        }

        if (strcmp(job_name, g_runtime.current_name) == 0) {
            g_runtime.status = status_processing;
            g_runtime.last_update = time(NULL);
        }

        (void)pthread_mutex_unlock(&g_runtime.lock);

        copied_bytes = 0U;
        copy_ok = (copy_file(job_upload, job_result, &copied_bytes) == 0);

        if (pthread_mutex_lock(&g_runtime.lock) != 0) {
            return NULL;
        }

        if (strcmp(job_name, g_runtime.current_name) == 0) {
            if (copy_ok != 0) {
                g_runtime.result_bytes = copied_bytes;
                g_runtime.status = status_ready;
            } else {
                g_runtime.status = status_error;
            }
            g_runtime.last_update = time(NULL);
        }

        if (copy_ok != 0) {
            g_runtime.jobs_completed++;
        } else {
            g_runtime.jobs_failed++;
        }
        g_runtime.last_update = time(NULL);
        (void)pthread_mutex_unlock(&g_runtime.lock);
    }

    return NULL;
}

int runtime_init(const struct contextserver *context)
{
    int create_result;

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    /* salvam directorul de output din config, pentru a construi fisierele .upload/.wav */
    if (safe_copy(g_runtime.output_dir, sizeof(g_runtime.output_dir), context->setari.directoroutput) != 0) {
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    /* resetam toata starea runtime, pentru pornire determinista a serverului */
    g_runtime.current_name[0] = '\0';
    g_runtime.upload_path[0] = '\0';
    g_runtime.result_path[0] = '\0';
    g_runtime.expected_bytes = 0U;
    g_runtime.received_bytes = 0U;
    g_runtime.result_bytes = 0U;
    g_runtime.next_chunk_index = 0U;
    g_runtime.status = status_idle;
    if (g_runtime.upload_fd >= 0) {
        (void)close(g_runtime.upload_fd);
    }
    g_runtime.upload_fd = -1;
    g_runtime.total_remote_clients = 0UL;
    g_runtime.active_remote_clients = 0UL;
    g_runtime.total_admin_clients = 0UL;
    g_runtime.active_admin_clients = 0UL;
    g_runtime.remote_requests = 0UL;
    g_runtime.admin_requests = 0UL;
    g_runtime.jobs_completed = 0UL;
    g_runtime.jobs_failed = 0UL;
    g_runtime.total_bytes_in = 0U;
    g_runtime.total_bytes_out = 0U;
    g_runtime.last_update = time(NULL);
    g_runtime.shutdown_requested = 0;
    g_runtime.queue_head = 0;
    g_runtime.queue_tail = 0;
    g_runtime.queue_count = 0;
    g_runtime.is_initialized = 1;
    if (pthread_cond_init(&g_runtime.queue_cond, NULL) != 0) {
        g_runtime.is_initialized = 0;
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }
    g_runtime.worker_started = 0;

    (void)pthread_mutex_unlock(&g_runtime.lock);

    create_result = pthread_create(&g_runtime.worker_thread, NULL, runtime_worker_main, NULL); /* pornim workerul, pentru procesare coada */
    if (create_result != 0) {
        if (pthread_mutex_lock(&g_runtime.lock) == 0) {
            g_runtime.is_initialized = 0;
            (void)pthread_mutex_unlock(&g_runtime.lock);
        }
        (void)pthread_cond_destroy(&g_runtime.queue_cond);
        return -1;
    }

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }
    g_runtime.worker_started = 1;
    (void)pthread_mutex_unlock(&g_runtime.lock);
    return 0;
}

int runtime_destroy(void)
{
    int need_join;

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    /* daca runtime-ul nu e initializat, iesim curat, pentru apeluri idempotente */
    if (g_runtime.is_initialized == 0) {
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return 0;
    }

    g_runtime.shutdown_requested = 1; /* semnalam oprirea, pentru shutdown curat al workerului */
    g_runtime.last_update = time(NULL);
    need_join = g_runtime.worker_started;
    if (pthread_cond_signal(&g_runtime.queue_cond) != 0) {
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }
    (void)pthread_mutex_unlock(&g_runtime.lock);

    /* asteptam workerul, pentru a nu lasa thread-uri active la shutdown */
    if (need_join != 0 && pthread_join(g_runtime.worker_thread, NULL) != 0) {
        return -1;
    }

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }
    g_runtime.worker_started = 0;
    g_runtime.is_initialized = 0;
    if (g_runtime.upload_fd >= 0) {
        (void)close(g_runtime.upload_fd);
        g_runtime.upload_fd = -1;
    }
    if (pthread_cond_destroy(&g_runtime.queue_cond) != 0) {
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }
    (void)pthread_mutex_unlock(&g_runtime.lock);
    return 0;
}

void runtime_remote_connected(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    g_runtime.total_remote_clients++; /* total istoric, pentru raportare */
    g_runtime.active_remote_clients++;
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

void runtime_remote_disconnected(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    if (g_runtime.active_remote_clients > 0UL) {
        g_runtime.active_remote_clients--;
    }
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

void runtime_admin_connected(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    g_runtime.total_admin_clients++;
    g_runtime.active_admin_clients++;
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

void runtime_admin_disconnected(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    if (g_runtime.active_admin_clients > 0UL) {
        g_runtime.active_admin_clients--;
    }
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

void runtime_remote_request(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    g_runtime.remote_requests++;
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

void runtime_admin_request(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    g_runtime.admin_requests++;
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

void runtime_request_shutdown(void)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    g_runtime.shutdown_requested = 1; /* setam flag global, pentru oprirea buclelor server */
    g_runtime.last_update = time(NULL);
    (void)pthread_cond_signal(&g_runtime.queue_cond);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

int runtime_shutdown_requested(void)
{
    int requested;

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return 0;
    }

    requested = g_runtime.shutdown_requested;
    (void)pthread_mutex_unlock(&g_runtime.lock);
    return requested;
}

int runtime_begin_upload(const char *composition_name, size_t total_bytes, char *error_text, size_t error_size)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    /* blocam upload-ul daca runtime-ul nu e gata, pentru a evita stari invalide */
    if (g_runtime.is_initialized == 0) {
        (void)snprintf(error_text, error_size, "server_not_initialized");
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    /* construim numele/calea jobului activ, pentru identificare unica in sesiune */
    if (safe_copy(g_runtime.current_name, sizeof(g_runtime.current_name), composition_name) != 0 ||
        build_path(g_runtime.upload_path, sizeof(g_runtime.upload_path), g_runtime.output_dir, composition_name, ".upload") != 0 ||
        build_path(g_runtime.result_path, sizeof(g_runtime.result_path), g_runtime.output_dir, composition_name, ".wav") != 0) {
        (void)snprintf(error_text, error_size, "composition_name_too_long");
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    /* inchidem un upload precedent deschis, pentru a preveni descriptor leak */
    if (g_runtime.upload_fd >= 0) {
        (void)close(g_runtime.upload_fd);
    }

    g_runtime.upload_fd = open(g_runtime.upload_path, O_CREAT | O_WRONLY | O_TRUNC, file_permissions);
    if (g_runtime.upload_fd < 0) {
        (void)snprintf(error_text, error_size, "cannot_open_upload_file");
        g_runtime.status = status_error;
        g_runtime.jobs_failed++;
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    /* initializam progresul de upload, pentru raportare STATUS corecta */
    g_runtime.expected_bytes = total_bytes;
    g_runtime.received_bytes = 0U;
    g_runtime.result_bytes = 0U;
    g_runtime.next_chunk_index = 0U;
    g_runtime.status = status_uploading;
    g_runtime.last_update = time(NULL);

    if (total_bytes == 0U) {
        /* job fara payload: il punem direct in coada, pentru flux uniform */
        g_runtime.status = status_queued;
        if (g_runtime.queue_count >= queue_capacity) {
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            (void)snprintf(error_text, error_size, "queue_full");
            (void)pthread_mutex_unlock(&g_runtime.lock);
            return -1;
        }

        if (close(g_runtime.upload_fd) < 0) {
            g_runtime.upload_fd = -1;
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            (void)snprintf(error_text, error_size, "close_upload_failed");
            (void)pthread_mutex_unlock(&g_runtime.lock);
            return -1;
        }
        g_runtime.upload_fd = -1;

        /* punem jobul direct in FIFO, pentru a pastra acelasi flux ca la upload normal */
        if (safe_copy(g_runtime.queue_names[g_runtime.queue_tail], sizeof(g_runtime.queue_names[g_runtime.queue_tail]), g_runtime.current_name) != 0 ||
            safe_copy(g_runtime.queue_upload_paths[g_runtime.queue_tail], sizeof(g_runtime.queue_upload_paths[g_runtime.queue_tail]), g_runtime.upload_path) != 0 ||
            safe_copy(g_runtime.queue_result_paths[g_runtime.queue_tail], sizeof(g_runtime.queue_result_paths[g_runtime.queue_tail]), g_runtime.result_path) != 0) {
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            (void)snprintf(error_text, error_size, "queue_enqueue_failed");
            (void)pthread_mutex_unlock(&g_runtime.lock);
            return -1;
        }

        g_runtime.queue_tail = (g_runtime.queue_tail + 1) % queue_capacity;
        g_runtime.queue_count++;
        (void)pthread_cond_signal(&g_runtime.queue_cond);
    }

    (void)pthread_mutex_unlock(&g_runtime.lock);
    return 0;
}

int runtime_append_chunk(size_t chunk_index, const unsigned char *data, size_t chunk_bytes, char *error_text, size_t error_size)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    /* acceptam chunk doar in sesiune de upload activa, pentru consistenta protocolului */
    if (g_runtime.status != status_uploading || g_runtime.upload_fd < 0) {
        (void)snprintf(error_text, error_size, "no_active_upload");
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    if (chunk_index != g_runtime.next_chunk_index) {
        /* ordinea e stricta, pentru integritatea fisierului upload */
        (void)snprintf(error_text, error_size, "unexpected_chunk_index");
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    /* scriem payload-ul brut in fisierul .upload, pentru reconstructie completa */
    if (chunk_bytes > 0U && write_all(g_runtime.upload_fd, data, chunk_bytes) != 0) {
        (void)snprintf(error_text, error_size, "chunk_write_failed");
        (void)close(g_runtime.upload_fd);
        g_runtime.upload_fd = -1;
        g_runtime.status = status_error;
        g_runtime.jobs_failed++;
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    g_runtime.received_bytes += chunk_bytes;
    g_runtime.total_bytes_in += chunk_bytes;
    g_runtime.next_chunk_index++;
    g_runtime.last_update = time(NULL);

    /* protejam depasirea marimii declarate, pentru integritatea upload-ului */
    if (g_runtime.received_bytes > g_runtime.expected_bytes) {
        (void)snprintf(error_text, error_size, "received_more_than_expected");
        (void)close(g_runtime.upload_fd);
        g_runtime.upload_fd = -1;
        g_runtime.status = status_error;
        g_runtime.jobs_failed++;
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    if (g_runtime.received_bytes == g_runtime.expected_bytes) {
        /* upload complet: mutam jobul in coada, pentru procesare async */
        if (g_runtime.queue_count >= queue_capacity) {
            (void)snprintf(error_text, error_size, "queue_full");
            (void)close(g_runtime.upload_fd);
            g_runtime.upload_fd = -1;
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            (void)pthread_mutex_unlock(&g_runtime.lock);
            return -1;
        }

        if (close(g_runtime.upload_fd) < 0) {
            g_runtime.upload_fd = -1;
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            (void)snprintf(error_text, error_size, "close_upload_failed");
            (void)pthread_mutex_unlock(&g_runtime.lock);
            return -1;
        }
        g_runtime.upload_fd = -1;
        g_runtime.status = status_queued; /* upload inchis, job gata de procesare */
        if (safe_copy(g_runtime.queue_names[g_runtime.queue_tail], sizeof(g_runtime.queue_names[g_runtime.queue_tail]), g_runtime.current_name) != 0 ||
            safe_copy(g_runtime.queue_upload_paths[g_runtime.queue_tail], sizeof(g_runtime.queue_upload_paths[g_runtime.queue_tail]), g_runtime.upload_path) != 0 ||
            safe_copy(g_runtime.queue_result_paths[g_runtime.queue_tail], sizeof(g_runtime.queue_result_paths[g_runtime.queue_tail]), g_runtime.result_path) != 0) {
            g_runtime.status = status_error;
            g_runtime.jobs_failed++;
            (void)snprintf(error_text, error_size, "queue_enqueue_failed");
            (void)pthread_mutex_unlock(&g_runtime.lock);
            return -1;
        }

        g_runtime.queue_tail = (g_runtime.queue_tail + 1) % queue_capacity;
        g_runtime.queue_count++;
        (void)pthread_cond_signal(&g_runtime.queue_cond);
    }

    (void)pthread_mutex_unlock(&g_runtime.lock);
    return 0;
}

int runtime_get_status(const char *composition_name, char *status_text, size_t status_size, size_t *received, size_t *expected) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    int found;

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    /* expunem status doar pentru jobul curent, pentru modelul simplu milestone 2 */
    found = (strcmp(composition_name, g_runtime.current_name) == 0);
    if (found == 0) {
        (void)snprintf(status_text, status_size, "NOT_FOUND");
        *received = 0U;
        *expected = 0U;
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    (void)snprintf(status_text, status_size, "%s", status_to_text(g_runtime.status));
    *received = g_runtime.received_bytes;
    *expected = g_runtime.expected_bytes;
    (void)pthread_mutex_unlock(&g_runtime.lock);
    return 0;
}

int runtime_get_result(const char *composition_name, char *result_path, size_t path_size, size_t *result_size)
{
    int is_ready;

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    /* rezultatul e disponibil doar cand jobul curent e READY */
    is_ready = (strcmp(composition_name, g_runtime.current_name) == 0 && g_runtime.status == status_ready);
    if (is_ready == 0 || safe_copy(result_path, path_size, g_runtime.result_path) != 0) {
        (void)pthread_mutex_unlock(&g_runtime.lock);
        return -1;
    }

    *result_size = g_runtime.result_bytes;
    (void)pthread_mutex_unlock(&g_runtime.lock);
    return 0;
}

void runtime_mark_bytes_out(size_t bytes)
{
    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return;
    }

    g_runtime.total_bytes_out += bytes; /* contorizam trafic iesire, pentru STATS admin */
    g_runtime.last_update = time(NULL);
    (void)pthread_mutex_unlock(&g_runtime.lock);
}

int runtime_admin_line(const struct contextserver *context, const char *command, char *output, size_t output_size)
{
    int written;
    const char *status_text;
    struct tm local_time;
    char time_buffer[64];

    if (pthread_mutex_lock(&g_runtime.lock) != 0) {
        return -1;
    }

    status_text = status_to_text(g_runtime.status); /* mapare status intern -> text protocol */
    if (localtime_r(&g_runtime.last_update, &local_time) != NULL) {
        if (strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &local_time) == 0U) {
            (void)snprintf(time_buffer, sizeof(time_buffer), "n/a");
        }
    } else {
        (void)snprintf(time_buffer, sizeof(time_buffer), "n/a");
    }

    /* fiecare ramura produce o singura linie text, pentru clientul admin */
    if (strcmp(command, "INFO") == 0) {
        written = snprintf(output, output_size, "OK INFO title=%s ordinary_port=%d admin_socket=%s output_dir=%s",
                           context->setari.titlu,
                           context->setari.portclienti,
                           context->setari.socketsadmin,
                           context->setari.directoroutput);
    } else if (strcmp(command, "CLIENTS") == 0) {
        written = snprintf(output, output_size, "OK CLIENTS remote_active=%lu remote_total=%lu admin_active=%lu admin_total=%lu",
                           g_runtime.active_remote_clients,
                           g_runtime.total_remote_clients,
                           g_runtime.active_admin_clients,
                           g_runtime.total_admin_clients);
    } else if (strcmp(command, "JOBS") == 0) {
        written = snprintf(output, output_size, "OK JOBS name=%s status=%s received=%zu expected=%zu",
                           g_runtime.current_name[0] == '\0' ? "-" : g_runtime.current_name,
                           status_text,
                           g_runtime.received_bytes,
                           g_runtime.expected_bytes);
    } else if (strcmp(command, "QUEUE") == 0) {
        int queue_length;

        queue_length = g_runtime.queue_count;
        if (g_runtime.status == status_uploading || g_runtime.status == status_processing) {
            /* includem si jobul activ, pentru perceptie corecta asupra incarcarii */
            queue_length++;
        }
        written = snprintf(output, output_size, "OK QUEUE length=%d",
                           queue_length);
    } else if (strcmp(command, "API") == 0) {
        written = snprintf(output, output_size, "OK API port=%d status=RUNNING",
                           context->setari.portapi);
    } else if (strcmp(command, "STATS") == 0) {
        written = snprintf(output, output_size, "OK STATS remote_requests=%lu admin_requests=%lu bytes_in=%zu bytes_out=%zu",
                           g_runtime.remote_requests,
                           g_runtime.admin_requests,
                           g_runtime.total_bytes_in,
                           g_runtime.total_bytes_out);
    } else if (strcmp(command, "HISTORY") == 0) {
        written = snprintf(output, output_size, "OK HISTORY done=%lu failed=%lu last_update=%s",
                           g_runtime.jobs_completed,
                           g_runtime.jobs_failed,
                           time_buffer);
    } else if (strcmp(command, "SHUTDOWN") == 0) {
        /* comanda de oprire controlata, pentru terminare eleganta din admin */
        g_runtime.shutdown_requested = 1;
        g_runtime.last_update = time(NULL);
        written = snprintf(output, output_size, "OK SHUTTING_DOWN");
    } else {
        written = snprintf(output, output_size, "ERR UNKNOWN_ADMIN_COMMAND");
    }

    (void)pthread_mutex_unlock(&g_runtime.lock);
    if (written < 0 || (size_t)written >= output_size) {
        return -1;
    }

    return 0;
}
