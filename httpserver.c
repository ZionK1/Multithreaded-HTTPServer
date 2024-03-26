// CITE @asgn4-starter code from resources/practica
#include "asgn2_helper_funcs.h"
#include "connection.h"
#include "response.h"
#include "request.h"
#include "queue.h"
#include "rwlock.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <pthread.h>

#define OPTIONS "t:"

// ---------------------------- structs from TA Mitchell's Pseudocode ------------------------------
typedef struct rwlockHTNodeObj {
    char *uri;
    rwlock_t *rwlock;
    struct rwlockHTNodeObj *next;
} rwlockHTNodeObj;

typedef rwlockHTNodeObj *rwlockHTNode;

typedef struct rwlockHTObj {
    int size;
    rwlockHTNode *array;
} rwlockHTObj;

typedef rwlockHTObj *rwlockHT;

typedef struct ThreadObj {
    pthread_t thread;
    int id;
    rwlockHT rwlock_ht;
} ThreadObj;

typedef ThreadObj *Thread;
// -------------------------------------------------------------------------------------------------

void handle_connection(int);
void handle_get(conn_t *);
void handle_put(conn_t *);
/*
void handle_connection(int, Thread);
void handle_get(conn_t *, Thread);
void handle_put(conn_t *, Thread);
*/
void handle_unsupported(conn_t *);

rwlockHT new_rwlockHT(int t) {
    // alloc HT itself
    rwlockHT newrwlockHT = (rwlockHT) malloc(sizeof(struct rwlockHTObj));
    if (newrwlockHT == NULL) {
        return NULL;
    }

    // set size
    newrwlockHT->size = t;

    // alloc linked map of Nodes
    newrwlockHT->array = (rwlockHTNode *) malloc(t * sizeof(rwlockHTNode));
    for (int i = 0; i < t; ++i) {
        newrwlockHT->array[i] = NULL;
    }

    return newrwlockHT;
}

// ------------------------------------------ globals -------------------------------------------------
queue_t *q;
sem_t mutex;
rwlockHT rwlockHT_global;

// CITE @Mitchell Code
// worker: takes connection and handles it accordingly
//void workerThread(Thread thread) {
void workerThread() {
    while (1) {
        //fprintf(stderr, "WORKER LOOP----------------------\n");
        uintptr_t fd = 0; // init var in NULL state (from ChatGPT)
        queue_pop(q, (void **) &fd); // get fd from queue (from ChatGPT)
        //fprintf(stderr, "POPPED---------------------------\n");
        handle_connection(fd);
        close(fd);
        //fprintf(stderr, "closed fd-----------------------\n");
    }
}

// --------------------------------------------- main ---------------------------------------------

int main(int argc, char **argv) {
    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[3]);
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[3], &endptr, 10);
    //fprintf(stderr, "GOT PORT\n");

    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[3]);
        return EXIT_FAILURE;
    }

    // default thread count of 4
    int t = 4;

    // use getopt to specify thread count
    int opt = 0;
    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't':
            t = strtoul(optarg, NULL, 10);
            //fprintf(stderr, "THREADS SET\n");
            break;
        default: fprintf(stderr, "Bad Input\n");
        }
    }
    //fprintf(stderr, "main ---- t = %d\n", t);

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    // init queue
    q = queue_new(t);

    // init mutex
    sem_init(&mutex, 0, 1);

    // init threads
    Thread threads[t];
    rwlockHT_global = new_rwlockHT(t);

    for (int i = 0; i < t; i++) {
        threads[i] = (Thread) malloc(sizeof(struct ThreadObj));
        threads[i]->id = i;
        threads[i]->rwlock_ht = rwlockHT_global;
        pthread_create(&threads[i]->thread, NULL, (void *(*) (void *) ) workerThread, NULL);
    }

    //fprintf(stderr, "threads made\n");

    // dispatcher: push to workerThreads, blocks if full
    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        //fprintf(stderr, "dispatcher             accepted conn..........\n");
        queue_push(q, (void *) connfd);
        //fprintf(stderr, "dispatcher             pushed conn.............\n");
    }

    return EXIT_SUCCESS;
}

//void handle_connection(int connfd, Thread thread) {
void handle_connection(int connfd) {
    conn_t *conn = conn_new(connfd);
    //fprintf(stderr, "NEW CONNECTION ---------------------\n");

    const Response_t *res = conn_parse(conn);
    //fprintf(stderr, "handle_conn -------- done parsing\n");

    if (res != NULL) {
        conn_send_response(conn, res);
        //fprintf(stderr, "handle_conn -------- res != NULL\n");
    } else {
        //fprintf(stderr, "handle_conn -------- res == NULL\n");
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            //fprintf(stderr, "handle_conn -------- GET\n");
            //handle_get(conn, thread);
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            //fprintf(stderr, "handle_conn -------- PUT\n");
            //handle_put(conn, thread);
            handle_put(conn);
        } else {
            //fprintf(stderr, "handle_conn -------- UNSUPPORTED\n");
            handle_unsupported(conn);
        }
    }

    conn_delete(&conn);

    //fprintf(stderr, "EXITING CONNECTION --------------------\n");
}

//void handle_get(conn_t *conn, Thread thread) {
void handle_get(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    //fprintf(stderr, "handle_get ---- uri: %s\n", uri);
    const Response_t *res = NULL;

    char *req_id = conn_get_header(conn, "Request-Id");
    if (!req_id) {
        req_id = "0";
    }
    //fprintf(stderr, "handle_get ---- Request-Id: %s\n", req_id);

    // ---------------------------------- START OF HT Code -------------------------------------
    // design:
    // check if uri exists in HT
    // if it doesnt, create a new lock and add node to HT
    // else, check lock condition to proceed

    // lock this part
    sem_wait(&mutex);

    // try to get rwlock for uri (hash function from ChatGPT)
    int index = 0;
    while (*uri) {
        index += *uri;
        uri++;
    }
    index = index % (rwlockHT_global->size);

    // reset uri
    uri = conn_get_uri(conn);

    // search for HTNode (from ChatGPT)
    int found = 0;
    rwlockHTNode current = rwlockHT_global->array[index];
    while (current != NULL) {
        if (strcmp(current->uri, uri) == 0) {
            found = 1;
            break;
        }
        current = current->next;
    }

    // ---------------------------------- END OF HT Code -------------------------------------

    // What are the steps in here?

    // 1. Open the file.
    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES) {
            res = &RESPONSE_FORBIDDEN;
            goto out;
        } else if (errno == ENOENT) {
            res = &RESPONSE_NOT_FOUND;
            goto out;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            goto out;
        }
    }

    // 2. Get the size of the file.
    struct stat st;
    fstat(fd, &st);
    off_t file_size = st.st_size;

    // 3. Check if the file is a directory, because directories *will*
    // open, but are not valid.
    if (S_ISDIR(st.st_mode)) {
        res = &RESPONSE_FORBIDDEN;
        close(fd);
        goto out;
    }

    rwlockHTNode newNode = NULL;

    // lock file with rwlock
    if (found == 1) { // lock exists
        reader_lock(current->rwlock);
    } else { // create new lock
        // alloc node itself
        newNode = (rwlockHTNode) malloc(sizeof(struct rwlockHTNodeObj));

        // create new rwlock for uri
        rwlock_t *new_rwlock = rwlock_new(N_WAY, 1);

        // assign to newNode
        newNode->uri = uri;
        newNode->rwlock = new_rwlock;
        newNode->next = rwlockHT_global->array[index];

        //update HT
        rwlockHT_global->array[index] = newNode;

        // lock new rwlock
        reader_lock(newNode->rwlock);
    }

    // 4. Send the file
    conn_send_file(conn, fd, file_size);
    res = &RESPONSE_OK;
    close(fd);

    // unlock rwlock
    if (found == 1) {
        //fprintf(stderr, "[DEBUG] ----------- EXISTING RWUNLOCK\n");
        reader_unlock(current->rwlock);
    } else {
        //fprintf(stderr, "[DEBUG] ----------- NEW RWUNLOCK\n");
        reader_unlock(newNode->rwlock);
    }

    // after done with lock, delete HT Node ------- continue writing

out:
    //conn_send_response(conn, res);

    if (res == &RESPONSE_FORBIDDEN) {
        fprintf(stderr, "GET,/%s,403,%s\n", uri, req_id);
        conn_send_response(conn, res);
    } else if (res == &RESPONSE_NOT_FOUND) {
        fprintf(stderr, "GET,/%s,404,%s\n", uri, req_id);
        conn_send_response(conn, res);
    } else if (res == &RESPONSE_OK) {
        fprintf(stderr, "GET,/%s,200,%s\n", uri, req_id);
    } else if (res == &RESPONSE_INTERNAL_SERVER_ERROR) {
        fprintf(stderr, "GET,/%s,500,%s\n", uri, req_id);
        conn_send_response(conn, res);
    }

    // unlock mutex
    sem_post(&mutex);
}

//void handle_put(conn_t *conn, Thread thread) {
void handle_put(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    //fprintf(stderr, "handle_put ---- uri: %s\n", uri);
    const Response_t *res = NULL;

    char *req_id = conn_get_header(conn, "Request-Id");
    if (!req_id) {
        req_id = "0";
    }
    //fprintf(stderr, "handle_put ---- Request-Id: %s\n", req_id);

    // ---------------------------------- START OF HT Code -------------------------------------

    // lock this part
    sem_wait(&mutex);

    // try to get rwlock for uri (hash function from ChatGPT)
    int index = 0;
    while (*uri) {
        index += *uri;
        uri++;
    }
    index = index % (rwlockHT_global->size);

    // reset uri
    uri = conn_get_uri(conn);

    //fprintf(stderr, "handle_put ---- got index\n");
    //fprintf(stderr, "handle_put ---- index = %d\n", index);

    int found = 0;
    rwlockHTNode current = rwlockHT_global->array[index];
    while (current != NULL) {
        if (strcmp(current->uri, uri) == 0) {
            found = 1;
            break;
        }
        current = current->next;
    }
    //fprintf(stderr, "handle_put ---- searched for Node\n");

    // ------------------------------------ END OF HT Code ---------------------------------------

    //fprintf(stderr, "handle_put ---- MUTEX LOCK\n");

    // Check if file already exists before opening it.
    bool existed = access(uri, F_OK) == 0;
    //fprintf(stderr, "handle_put ---- FILE existed = %d\n", existed);

    // Open the file..
    int fd = open(uri, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
            res = &RESPONSE_FORBIDDEN;
            goto out;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            goto out;
        }
    }

    //fprintf(stderr, "handle_put ---- FILE created succesfully\n");

    // ---------------------------------- START OF HT Code -------------------------------------

    rwlockHTNode newNode = NULL;

    // lock file with rwlock
    if (found == 1) { // lock exists
        writer_lock(current->rwlock);
        //fprintf(stderr, "handle_put ---- WRITER LOCK\n");
    } else { // create new lock
        //fprintf(stderr, "handle_put ---- STARTING INSERT NEW NODE\n");
        // alloc node itself
        newNode = (rwlockHTNode) malloc(sizeof(struct rwlockHTNodeObj));
        if (newNode == NULL) {
            //fprintf(stderr, "handle_put ---- NODE MALLOC FAILED\n");
        }

        // create new rwlock for uri
        rwlock_t *new_rwlock = rwlock_new(N_WAY, 1);
        //fprintf(stderr, "handle_put ---- N_WAY RWLOCK CREATED\n");

        // assign to newNode
        newNode->uri = uri;
        newNode->rwlock = new_rwlock;
        newNode->next = NULL;

        //update HT
        rwlockHT_global->array[index] = newNode;
        //fprintf(stderr, "handle_put ---- NODE INSERTED\n");

        // lock new rwlock
        writer_lock(newNode->rwlock);
        //fprintf(stderr, "handle_put ---- NEW NODE WRITER LOCK\n");
    }

    // ------------------------------------ END OF HT Code ---------------------------------------

    res = conn_recv_file(conn, fd);
    //fprintf(stderr, "handle_put ---- CONN RECV FILE DONE\n");
    close(fd);
    //fprintf(stderr, "handle_put ---- FD CLOSED\n");

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
        //fprintf(stderr, "handle_put ---- RESPONSE OK\n");
    } else if (res == NULL && !existed) {
        res = &RESPONSE_CREATED;
        //fprintf(stderr, "handle_put ---- RESPONSE CREATED\n");
    }

    // unlock rwlock
    if (found == 1) {
        //fprintf(stderr, "handle_put ---- found == 1, unlock existed\n");
        writer_unlock(current->rwlock);
        //fprintf(stderr, "handle_put ---- EXISTING RWUNLOCK\n");
    } else {
        //fprintf(stderr, "handle_put ---- found == 0, unlock new\n");
        writer_unlock(newNode->rwlock);
        //fprintf(stderr, "handle_put ---- NEW RWUNLOCK\n");
    }

out:
    conn_send_response(conn, res);

    if (res == &RESPONSE_FORBIDDEN) {
        fprintf(stderr, "PUT,/%s,403,%s\n", uri, req_id);
    } else if (res == &RESPONSE_NOT_FOUND) {
        fprintf(stderr, "PUT,/%s,404,%s\n", uri, req_id);
    } else if (res == &RESPONSE_OK) {
        fprintf(stderr, "PUT,/%s,200,%s\n", uri, req_id);
    } else if (res == &RESPONSE_CREATED) {
        fprintf(stderr, "PUT,/%s,201,%s\n", uri, req_id);
    } else if (res == &RESPONSE_INTERNAL_SERVER_ERROR) {
        fprintf(stderr, "PUT,/%s,500,%s\n", uri, req_id);
    }

    // unlock mutex
    sem_post(&mutex);
    //fprintf(stderr, "handle_put ---- MUTEX UNLOCK\n");
}

void handle_unsupported(conn_t *conn) {

    // placeholder, checking conn_send_response stuff
    if (conn) {
    }

    // send response
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
    //fprintf(stderr, "
}
