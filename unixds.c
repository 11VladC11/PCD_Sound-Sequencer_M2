/*
 * Cojocaru Vlad
 * IR3 - grupa 4
 * Proiect curs - T32 sound sequencer - unixds
 * implementeaza canalul UNIX pentru operatii simple de administrare
 */

#include <errno.h> /* errno/EINTR/EAGAIN, pentru tratarea corecta a erorilor */
#include <stddef.h> /* size_t, pentru operatii de buffer */
#include <stdio.h> /* printf/perror, pentru loguri si diagnostic */
#include <string.h> /* strlen/strcmp/memset/memcpy, pentru protocol si adrese */
#include <sys/select.h> /* select/fd_set, pentru polling al socketului de listen */
#include <sys/socket.h> /* socket/bind/listen/accept/setsockopt, pentru server UNIX */
#include <sys/time.h> /* timeval, pentru timeouturi de citire/polling */
#include <sys/un.h> /* sockaddr_un/AF_UNIX, pentru endpoint local admin */
#include <unistd.h> /* read/write/close, pentru I/O pe conexiunile admin */

#include "proto.h" /* contextserver, pentru acces la socketul si timeout-ul din config */
#include "runtime_state.h" /* statistici/comenzi admin/shutdown, pentru integrarea cu runtime */

enum {
    /* backlog suficient pentru conexiuni admin punctuale, pentru a evita refuzuri la connect */
    listen_backlog = 8,
    /* buffer de linie pentru comenzi/rspunsuri scurte, pentru a pastra protocolul simplu */
    line_size = 1024
};

/* scrie exact <lungime> octeti in socket-ul clientului admin, pentru a nu trimite raspunsuri trunchiate */
static int scrietot(int descriptor, const char *buffer, size_t lungime)
{
    size_t scris;

    scris = 0U;
    while (scris < lungime) {
        ssize_t rezultat;

        rezultat = write(descriptor, buffer + scris, lungime - scris);
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

/* helper simplu pentru string terminat cu '\0', pentru a reduce duplicarea codului */
static int scrietext(int descriptor, const char *text)
{
    return scrietot(descriptor, text, strlen(text));
}

/*
 * citeste o linie text terminata cu '\n' din socket.
 * coduri de retur:
 *   0  -> linie citita cu succes
 *   1  -> EOF curat (clientul a inchis conexiunea)
 *   2  -> timeout de receive (SO_RCVTIMEO), folosim pentru polling shutdown
 *  -1  -> eroare reala de I/O
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

/*
 * bucla de protocol pentru un client admin conectat pe UNIX socket.
 * comenzi asteptate:
 *   - QUIT (inchide sesiunea curenta)
 *   - INFO/CLIENTS/JOBS/QUEUE/API/STATS/HISTORY/SHUTDOWN
 * restul comenzilor sunt tratate in runtime_admin_line.
 */
static int gestioneazaclientadmin(int descriptorclient, const struct contextserver *context)
{
    char linie[line_size];

    /* marcam intrarea clientului in statistici, pentru a avea metrici corecte in admin */
    runtime_admin_connected();
    while (1) {
        int rezultatcitire;

        /* citire cu timeout scurt, pentru a putea iesi elegant la shutdown */
        rezultatcitire = citestelinie(descriptorclient, linie, sizeof(linie));
        if (rezultatcitire == 2) {
            /* pe timeout, nu e eroare, pentru a continua sesiunea cat timp serverul e activ */
            if (runtime_shutdown_requested() != 0) {
                break;
            }
            continue;
        }
        if (rezultatcitire > 0) {
            /* EOF: clientul a inchis conexiunea, pentru a termina sesiunea curenta */
            break;
        }
        if (rezultatcitire < 0) {
            /* eroare I/O: inchidem sesiunea cu status de eroare, pentru a evita stare inconsitenta */
            runtime_admin_disconnected();
            return -1;
        }

        /* orice linie valida e considerata request admin, pentru contorizare si audit */
        runtime_admin_request();
        if (strcmp(linie, "QUIT") == 0) {
            /* inchidere explicita a sesiunii admin, pentru terminare controlata a conexiunii */
            if (scrietext(descriptorclient, "OK BYE\n") != 0) {
                runtime_admin_disconnected();
                return -1;
            }
            break;
        }

        {
            char raspuns[line_size];

            /*
             * runtime_admin_line construieste raspunsul textual,
             * iar aici il trimitem ca linie terminata cu '\n', pentru compatibilitate cu clientul.
             */
            if (runtime_admin_line(context, linie, raspuns, sizeof(raspuns)) != 0 ||
                scrietext(descriptorclient, raspuns) != 0 ||
                scrietext(descriptorclient, "\n") != 0) {
                runtime_admin_disconnected();
                return -1;
            }
        }
    }

    runtime_admin_disconnected();
    return 0;
}

/*
 * thread-ul serverului admin UNIX.
 * creeaza socket local -> bind -> listen -> accept in bucla.
 * foloseste select cu timeout 1s pentru a permite shutdown cooperativ.
 */
void *unix_main(void *args)
{
    const struct contextserver *context;
    struct sockaddr_un adresa;
    int descriptorascultare;

    context = (const struct contextserver *)args;
    if (printf("fir unix admin: socket %s, timeout %d secunde\n",
               context->setari.socketsadmin,
               context->setari.timeoutadmin) < 0) {
        perror("printf");
        return (void *)1;
    }

    /* endpoint local AF_UNIX pentru clientul de administrare, pentru comunicare strict locala */
    descriptorascultare = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptorascultare < 0) {
        perror("socket");
        return (void *)1;
    }

    /* construim adresa UNIX din calea din config, pentru a folosi exact socketul setat de utilizator */
    memset(&adresa, 0, sizeof(adresa));
    adresa.sun_family = AF_UNIX;
    /* validam dimensiunea caii in bufferul kernel pentru sun_path, pentru a preveni overflow logic */
    if (strlen(context->setari.socketsadmin) >= sizeof(adresa.sun_path)) {
        (void)close(descriptorascultare);
        return (void *)1;
    }
    (void)memcpy(adresa.sun_path,
                 context->setari.socketsadmin,
                 strlen(context->setari.socketsadmin) + 1U);

    /* asociem descriptorul la calea UNIX, pentru a expune serviciul admin pe socketul local */
    if (bind(descriptorascultare, (const struct sockaddr *)&adresa, sizeof(adresa)) < 0) {
        perror("bind");
        (void)close(descriptorascultare);
        return (void *)1;
    }

    /* intram in modul server (pasiv), pentru a accepta conexiuni admin */
    if (listen(descriptorascultare, listen_backlog) < 0) {
        perror("listen");
        (void)close(descriptorascultare);
        return (void *)1;
    }

    /* bucla principala de accept pentru clienti admin, pentru a deservi cereri succesive */
    while (1) {
        int descriptorclient;
        int selectrezultat;
        fd_set setcitire;
        struct timeval timeout;

        /* select pe socketul de listen + timeout periodic, pentru polling de shutdown */
        FD_ZERO(&setcitire);
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

        /* shutdown cerut din runtime (de ex. comanda SHUTDOWN), pentru iesire curata din thread */
        if (runtime_shutdown_requested() != 0) {
            break;
        }
        /* timeout simplu: reluam bucla, pentru a nu bloca thread-ul inacceptabil de mult */
        if (selectrezultat == 0) {
            continue;
        }

        /* acceptam exact un client si il procesam sincron, pentru simplitate in milestone-ul curent */
        descriptorclient = accept(descriptorascultare, NULL, NULL);
        if (descriptorclient < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            (void)close(descriptorascultare);
            return (void *)1;
        }

        /* timeout pe read client, pentru ca citestelinie sa poata intoarce codul 2 la inactivitate */
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        if (setsockopt(descriptorclient, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            perror("setsockopt");
            (void)close(descriptorclient);
            (void)close(descriptorascultare);
            return (void *)1;
        }

        /* ruleaza protocolul text admin pentru conexiunea curenta, pentru comenzi de mentenanta */
        if (gestioneazaclientadmin(descriptorclient, context) != 0) {
            (void)close(descriptorclient);
            (void)close(descriptorascultare);
            return (void *)1;
        }

        /* inchidem conexiunea clientului dupa finalizarea sesiunii, pentru a elibera descriptorul */
        if (close(descriptorclient) < 0) {
            perror("close");
            (void)close(descriptorascultare);
            return (void *)1;
        }
    }

    /* inchidere normala a socketului de listen, pentru cleanup complet la terminare */
    if (close(descriptorascultare) < 0) {
        perror("close");
        return (void *)1;
    }

    return NULL;
}
