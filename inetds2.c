/*
 * Cojocaru Vlad
 * IR3 - grupa 4
 * Proiect curs - T32 sound sequencer - inetds2
 * implementeaza canalul INET pentru clientii ordinari si operatiile UPLOAD/CHUNK/STATUS/RESULT
 */

#include <arpa/inet.h> /* htons/htonl, pentru conversii endian in retea */
#include <errno.h> /* errno/EINTR/EAGAIN, pentru tratarea robusta a erorilor */
#include <fcntl.h> /* open, pentru citirea fisierului rezultat */
#include <netinet/in.h> /* sockaddr_in, pentru endpoint TCP IPv4 */
#include <stdint.h> /* uint16_t, pentru cast sigur la port */
#include <stdio.h> /* printf/perror/snprintf, pentru loguri si raspunsuri */
#include <stdlib.h> /* malloc/free, pentru bufferul CHUNK dinamic */
#include <string.h> /* strcmp/strncmp/memset/sscanf, pentru protocol text */
#include <sys/socket.h> /* socket/bind/listen/accept/setsockopt, pentru server TCP */
#include <sys/select.h> /* select/fd_set, pentru polling fara blocare lunga */
#include <sys/time.h> /* timeval, pentru timeout in select/recv */
#include <sys/stat.h> /* definitii stat, pentru compatibilitate API POSIX */
#include <unistd.h> /* read/write/close, pentru I/O pe socket si fisier */

#include "proto.h" /* contextserver/marimi comune, pentru configuratia serverului */
#include "runtime_state.h" /* stare upload/coada/statistici, pentru operatiile remote */

enum {
    /* backlog putin mai mare, pentru clienti remote care se conecteaza aproape simultan */
    listen_backlog = 16,
    /* linie text de protocol, pentru comenzi UPLOAD/CHUNK/STATUS/RESULT */
    line_size = 1024,
    /* limita defensiva pe chunk, pentru a evita alocari excesive intr-o singura comanda */
    chunk_limit = 1048576,
    /* buffer de streaming fisier rezultat, pentru transfer eficient */
    io_buffer_size = 65536
};

/* scrie exact <lungime> octeti, pentru a nu pierde date in transferul pe socket */
static int scrietot(int descriptor, const void *buffer, size_t lungime)
{
    size_t scris;

    scris = 0U;
    while (scris < lungime) {
        ssize_t rezultat;

        rezultat = write(descriptor, (const char *)buffer + scris, lungime - scris);
        if (rezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        scris += (size_t)rezultat;
    }

    return 0;
}

/* helper de text, pentru a trimite rapid antete si raspunsuri */
static int scrietext(int descriptor, const char *text)
{
    return scrietot(descriptor, text, strlen(text));
}

/* citeste exact numarul cerut de bytes, pentru payload-ul binar al CHUNK-ului */
static int citestetot(int descriptor, unsigned char *buffer, size_t lungime)
{
    size_t citit;

    citit = 0U;
    while (citit < lungime) {
        ssize_t rezultat;

        rezultat = read(descriptor, buffer + citit, lungime - citit);
        if (rezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (rezultat == 0) {
            return -1;
        }

        citit += (size_t)rezultat;
    }

    return 0;
}

/*
 * citeste o comanda text terminata cu '\n'.
 * returneaza 2 la timeout, pentru polling shutdown fara blocare lunga.
 */
static int citestelinie(int descriptor, char *linie, size_t marime)
{
    size_t pozitie;

    pozitie = 0U;
    while (pozitie + 1U < marime) {
        char caracter;
        ssize_t rezultat;

        rezultat = read(descriptor, &caracter, 1U);
        if (rezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 2;
            }
            return -1;
        }

        if (rezultat == 0) {
            if (pozitie == 0U) {
                return 1;
            }
            break;
        }

        if (caracter == '\n') {
            break;
        }

        if (caracter != '\r') {
            linie[pozitie] = caracter;
            pozitie++;
        }
    }

    linie[pozitie] = '\0';
    return 0;
}

/* trimite raspuns de eroare standardizat, pentru client remote */
static int trimiteraspunserror(int descriptor, const char *mesaj)
{
    if (scrietext(descriptor, "ERR ") != 0 ||
        scrietext(descriptor, mesaj) != 0 ||
        scrietext(descriptor, "\n") != 0) {
        return -1;
    }

    return 0;
}

/* trimite raspuns OK standardizat, pentru client remote */
static int trimiteraspunsok(int descriptor, const char *mesaj)
{
    if (scrietext(descriptor, "OK ") != 0 ||
        scrietext(descriptor, mesaj) != 0 ||
        scrietext(descriptor, "\n") != 0) {
        return -1;
    }

    return 0;
}

/* trimite fisierul rezultat catre client, pentru operatia RESULT */
/* proceseaza comanda RESULT, pentru a livra fisierul final in format streaming */
static int proceseazarezultat(int descriptor, const char *compozitie)
{
    char caleaudio[marimecale];
    char antet[line_size];
    size_t marimeaudio;
    int descriptorsursa;

    /* refuzam daca jobul nu e READY, pentru a pastra contractul protocolului */
    if (runtime_get_result(compozitie, caleaudio, sizeof(caleaudio), &marimeaudio) != 0) {
        return trimiteraspunserror(descriptor, "RESULT_NOT_READY");
    }

    descriptorsursa = open(caleaudio, O_RDONLY);
    if (descriptorsursa < 0) {
        return trimiteraspunserror(descriptor, "RESULT_OPEN_FAILED");
    }

    if (snprintf(antet, sizeof(antet), "RESULT %zu\n", marimeaudio) < 0 ||
        scrietext(descriptor, antet) != 0) {
        (void)close(descriptorsursa);
        return -1;
    }

    while (1) {
        unsigned char buffer[io_buffer_size];
        ssize_t rezultat;

        rezultat = read(descriptorsursa, buffer, sizeof(buffer));
        if (rezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(descriptorsursa);
            return -1;
        }

        if (rezultat == 0) {
            break;
        }

        if (scrietot(descriptor, buffer, (size_t)rezultat) != 0) {
            (void)close(descriptorsursa);
            return -1;
        }

        /* contorizam bytes out, pentru comanda STATS */
        runtime_mark_bytes_out((size_t)rezultat); 
    }

    if (close(descriptorsursa) < 0) {
        return -1;
    }

    return 0;
}

/* trateaza protocolul unui singur client remote, pentru sesiune sincronizata simpla */
static int gestioneazaclient(int descriptorclient)
{
    char linie[line_size];

    runtime_remote_connected(); /* marcam conectarea, pentru statistici admin */
    while (1) {
        int rezcitire;

        rezcitire = citestelinie(descriptorclient, linie, sizeof(linie)); /* timeout scurt, pentru shutdown cooperativ */
        if (rezcitire == 2) {
            if (runtime_shutdown_requested() != 0) {
                break;
            }
            continue;
        }
        if (rezcitire > 0) {
            break;
        }
        if (rezcitire < 0) {
            runtime_remote_disconnected();
            return -1;
        }

        runtime_remote_request();

        if (strncmp(linie, "UPLOAD ", 7) == 0) { /* pas 1 protocol: initializeaza jobul */
            char compozitie[marimetext];
            size_t total;
            char eroare[marimetext];

            if (sscanf(linie, "UPLOAD %127s %zu", compozitie, &total) != 2) {
                if (trimiteraspunserror(descriptorclient, "INVALID_UPLOAD") != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            if (runtime_begin_upload(compozitie, total, eroare, sizeof(eroare)) != 0) {
                if (trimiteraspunserror(descriptorclient, eroare) != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            if (trimiteraspunsok(descriptorclient, "UPLOAD_ACCEPTED") != 0) {
                runtime_remote_disconnected();
                return -1;
            }
            continue;
        }

        /* pas 2 protocol: transfer incremental payload */
        if (strncmp(linie, "CHUNK ", 6) == 0) { 
            size_t index;
            size_t marimechunk;
            unsigned char *buffer;
            char eroare[marimetext];

            if (sscanf(linie, "CHUNK %zu %zu", &index, &marimechunk) != 2 || marimechunk > chunk_limit) {
                if (trimiteraspunserror(descriptorclient, "INVALID_CHUNK") != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            buffer = NULL;
            if (marimechunk > 0U) {
                buffer = (unsigned char *)malloc(marimechunk); /* alocam exact payload-ul, pentru scriere corecta in upload */
                if (buffer == NULL) {
                    if (trimiteraspunserror(descriptorclient, "NO_MEMORY") != 0) {
                        runtime_remote_disconnected();
                        return -1;
                    }
                    continue;
                }

                if (citestetot(descriptorclient, buffer, marimechunk) != 0) {
                    free(buffer);
                    runtime_remote_disconnected();
                    return -1;
                }
            }

            if (runtime_append_chunk(index, buffer, marimechunk, eroare, sizeof(eroare)) != 0) {
                free(buffer);
                if (trimiteraspunserror(descriptorclient, eroare) != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            free(buffer);
            if (trimiteraspunsok(descriptorclient, "CHUNK_ACCEPTED") != 0) {
                runtime_remote_disconnected();
                return -1;
            }
            continue;
        }

        if (strncmp(linie, "STATUS ", 7) == 0) { /* pas 3 protocol: polling stare job */
            char compozitie[marimetext];
            char starea[marimetext];
            char mesaj[line_size];
            size_t primiti;
            size_t total;

            if (sscanf(linie, "STATUS %127s", compozitie) != 1) {
                if (trimiteraspunserror(descriptorclient, "INVALID_STATUS") != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            if (runtime_get_status(compozitie, starea, sizeof(starea), &primiti, &total) != 0 &&
                strcmp(starea, "NOT_FOUND") != 0) {
                if (trimiteraspunserror(descriptorclient, "STATUS_FAILED") != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            if (snprintf(mesaj, sizeof(mesaj), "STATUS %s %zu %zu", starea, primiti, total) < 0 ||
                trimiteraspunsok(descriptorclient, mesaj) != 0) {
                runtime_remote_disconnected();
                return -1;
            }
            continue;
        }

        if (strncmp(linie, "RESULT ", 7) == 0) { /* pas 4 protocol: descarcare rezultat */
            char compozitie[marimetext];

            if (sscanf(linie, "RESULT %127s", compozitie) != 1) {
                if (trimiteraspunserror(descriptorclient, "INVALID_RESULT") != 0) {
                    runtime_remote_disconnected();
                    return -1;
                }
                continue;
            }

            if (proceseazarezultat(descriptorclient, compozitie) != 0) {
                runtime_remote_disconnected();
                return -1;
            }
            continue;
        }

        if (strcmp(linie, "QUIT") == 0) { /* inchidere curata sesiune client */
            if (trimiteraspunsok(descriptorclient, "BYE") != 0) {
                runtime_remote_disconnected();
                return -1;
            }
            break;
        }

        if (trimiteraspunserror(descriptorclient, "UNKNOWN_COMMAND") != 0) {
            runtime_remote_disconnected();
            return -1;
        }
    }

    runtime_remote_disconnected();
    return 0;
}

/*
 * firul INET pentru clienti ordinari.
 * gestioneaza socket-ul TCP de listen si proceseaza conexiunile pe rand, pentru simplitate.
 */
void *inet_main(void *args)
{
    const struct contextserver *context;
    struct sockaddr_in adresa;
    int descriptorascultare;
    int optiune;

    context = (const struct contextserver *)args;
    if (printf("fir inet clienti ordinari: port %d\n", context->setari.portclienti) < 0) {
        perror("printf");
        return (void *)1;
    }

    descriptorascultare = socket(AF_INET, SOCK_STREAM, 0); /* endpoint TCP, pentru clientii remote */
    if (descriptorascultare < 0) {
        perror("socket");
        return (void *)1;
    }

    optiune = 1; /* REUSEADDR, pentru restart rapid dupa inchidere */
    if (setsockopt(descriptorascultare, SOL_SOCKET, SO_REUSEADDR, &optiune, sizeof(optiune)) < 0) {
        perror("setsockopt");
        (void)close(descriptorascultare);
        return (void *)1;
    }

    memset(&adresa, 0, sizeof(adresa));
    adresa.sin_family = AF_INET;
    adresa.sin_addr.s_addr = htonl(INADDR_ANY);
    adresa.sin_port = htons((uint16_t)context->setari.portclienti);

    if (bind(descriptorascultare, (const struct sockaddr *)&adresa, sizeof(adresa)) < 0) {
        perror("bind");
        (void)close(descriptorascultare);
        return (void *)1;
    }

    if (listen(descriptorascultare, listen_backlog) < 0) {
        perror("listen");
        (void)close(descriptorascultare);
        return (void *)1;
    }

    while (1) { /* bucla accept, pentru deservirea continua a clientilor */
        int descriptorclient;
        int selectrezultat;
        fd_set setcitire;
        struct timeval timeout;

        FD_ZERO(&setcitire); /* select cu timeout 1s, pentru verificare periodica shutdown */
        FD_SET(descriptorascultare, &setcitire);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        selectrezultat = select(descriptorascultare + 1, &setcitire, NULL, NULL, &timeout);
        if (selectrezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            (void)close(descriptorascultare);
            return (void *)1;
        }

        if (runtime_shutdown_requested() != 0) {
            break;
        }
        if (selectrezultat == 0) {
            continue;
        }

        descriptorclient = accept(descriptorascultare, NULL, NULL); /* acceptam client nou, pentru sesiune dedicata */
        if (descriptorclient < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            (void)close(descriptorascultare);
            return (void *)1;
        }

        timeout.tv_sec = 1; /* timeout pe recv client, pentru a nu bloca inchiderea serverului */
        timeout.tv_usec = 0;
        if (setsockopt(descriptorclient, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("setsockopt");
            (void)close(descriptorclient);
            (void)close(descriptorascultare);
            return (void *)1;
        }

        if (gestioneazaclient(descriptorclient) != 0) {
            (void)close(descriptorclient);
            (void)close(descriptorascultare);
            return (void *)1;
        }

        if (close(descriptorclient) < 0) {
            perror("close");
            (void)close(descriptorascultare);
            return (void *)1;
        }
    }

    if (close(descriptorascultare) < 0) {
        perror("close");
        return (void *)1;
    }

    return NULL;
}
