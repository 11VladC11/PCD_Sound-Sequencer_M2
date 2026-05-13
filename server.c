/*
 * Cojocaru Vlad
 * IR3 - grupa 4
 * Proiect curs - T32 sound sequencer - server
 * citeste configuratia comuna si construieste contextul de rulare al serverului
 * lanseaza firele unix, inet si ws api si asteapta terminarea lor
 * trateaza erori de config, pthread, output si curatarea socketului unix
 */

#include <pthread.h> /* fire separate, pentru a rula canalele serverului in paralel */
#include <signal.h> /* SIGPIPE */
#include <stdio.h> /* printf si perror la afisare */
#include <stdlib.h> /* constante standard */
#include <string.h> /* strlen si strerror pentru erori pthread */
#include <unistd.h> /* unlink, write pentru curatarea socketului unix si i/o */

#include "proto.h" /* API comun server, pentru config/context/fire componente */
#include "runtime_state.h" /* starea partajata runtime, pentru init/destroy si control shutdown */

/* scrie tot textul in descriptor, pentru a evita mesaje trunchiate la stderr/stdout */
static int scrietot(int descriptor, const char *text, size_t lungime)
{
    size_t scris;

    scris = 0;
    while (scris < lungime) {
        ssize_t rezultat;

        rezultat = write(descriptor, text + scris, lungime - scris);
        if (rezultat < 0) {
            perror("write");
            return -1;
        }

        scris += (size_t)rezultat;
    }

    return 0;
}

/* scrie un text complet in descriptor, pentru a simplifica apelurile repetitive */
static int scrietext(int descriptor, const char *text)
{
    return scrietot(descriptor, text, strlen(text));
}

/* copiaza un text cu verificare de marime, pentru a evita depasirea bufferului */
static int copiatext(char *destinatie, size_t marime, const char *sursa)
{
    size_t indice;

    indice = 0;
    while (sursa[indice] != '\0') {
        if (indice + 1 >= marime) {
            return -1;
        }

        destinatie[indice] = sursa[indice];
        indice++;
    }

    destinatie[indice] = '\0';
    return 0;
}

/* afiseaza o eroare de pthread fara perror, pentru ca pthread intoarce cod direct */
static int afiseazaeroarepthreads(const char *operatie, int cod)
{
    char text[marimetext];

    if (strerror_r(cod, text, sizeof(text)) != 0 &&
        copiatext(text, sizeof(text), "eroare pthread necunoscuta") != 0) {
        return -1;
    }

    if (scrietext(STDERR_FILENO, operatie) != 0 ||
        scrietext(STDERR_FILENO, ": ") != 0 ||
        scrietext(STDERR_FILENO, text) != 0 ||
        scrietext(STDERR_FILENO, "\n") != 0) {
        return -1;
    }

    return -1;
}

/* porneste firele unix, inet si ws api si asteapta doar ce a fost creat, pentru shutdown controlat */
static int ruleazaskeleton(const struct contextserver *context)
{
    pthread_t unixthr; /* NOLINT(misc-include-cleaner) */
    pthread_t inetthr; /* NOLINT(misc-include-cleaner) */
    pthread_t soapthr; /* NOLINT(misc-include-cleaner) */

    void *rezultat;
    int cod;
    int eroare;
    int areinet;
    int aresoap;

    eroare = 0;
    areinet = 0;
    aresoap = 0;

    /* sterg socketul vechi inainte de pornirea componentei unix, pentru a evita EADDRINUSE la bind */
    (void)unlink(context->setari.socketsadmin);

    cod = pthread_create(&unixthr, NULL, unix_main, (void *)context);
    if (cod != 0) {
        return afiseazaeroarepthreads("pthread_create unix_main", cod);
    }

    cod = pthread_create(&inetthr, NULL, inet_main, (void *)context);
    if (cod != 0) {
        eroare = 1;
        (void)afiseazaeroarepthreads("pthread_create inet_main", cod);
        goto cleanup;
    }
    areinet = 1;

    cod = pthread_create(&soapthr, NULL, soap_main, (void *)context);
    if (cod != 0) {
        eroare = 1;
        (void)afiseazaeroarepthreads("pthread_create soap_main", cod);
        goto cleanup;
    }
    aresoap = 1;

cleanup:
    /* chiar daca un create esueaza, fac join doar pe firele pornite corect, pentru cleanup sigur */
    /* NULL inseamna succes, orice alta valoare inseamna esec, pentru contract comun intre thread-uri */
    rezultat = NULL;
    cod = pthread_join(unixthr, &rezultat);
    if (cod != 0) {
        eroare = 1;
        (void)afiseazaeroarepthreads("pthread_join unix_main", cod);
    } else if (rezultat != NULL) {
        eroare = 1;
    }

    if (areinet != 0) {
        rezultat = NULL;
        cod = pthread_join(inetthr, &rezultat);
        if (cod != 0) {
            eroare = 1;
            (void)afiseazaeroarepthreads("pthread_join inet_main", cod);
        } else if (rezultat != NULL) {
            eroare = 1;
        }
    }

    if (aresoap != 0) {
        rezultat = NULL;
        cod = pthread_join(soapthr, &rezultat);
        if (cod != 0) {
            eroare = 1;
            (void)afiseazaeroarepthreads("pthread_join soap_main", cod);
        } else if (rezultat != NULL) {
            eroare = 1;
        }
    }
    /* sterge socketul, pentru a nu lasa endpoint stale dupa terminare */
    (void)unlink(context->setari.socketsadmin);
    return eroare == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    struct contextserver context;
    struct optiuniserver optiuni;
    int rezultatparse;

    /* evit oprirea procesului daca un client inchide socketul in timpul write, pentru stabilitate */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    if (initializeazaoptiuni(&optiuni) != 0) {
        return EXIT_FAILURE;
    }

    rezultatparse = parseazaoptiuni(argc, argv, &optiuni);
    if (rezultatparse < 0) {
        return EXIT_FAILURE;
    }

    if (rezultatparse > 0) {
        return afiseazaajutor() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* citesc contextul o singura data si il dau tuturor componentelor, pentru configuratie coerenta */
    if (incarca_context(optiuni.caleconfig, &context) != 0) {
        return EXIT_FAILURE;
    }

    /* directorul de output trebuie pregatit inainte de demo si rapoarte, pentru scriere fara erori */
    if (createdirector(context.setari.directoroutput) != 0) {
        return EXIT_FAILURE;
    }

    if (afiseazasumar(&context) != 0) {
        return EXIT_FAILURE;
    }

    if (runtime_init(&context) != 0) {
        return EXIT_FAILURE;
    }

    if (optiuni.afiseazamediu != 0 && afiseazamediu() != 0) {
        (void)runtime_destroy();
        return EXIT_FAILURE;
    }

    /* fara -d verific doar configuratia si afisez sumarul, pentru validare rapida a setup-ului */
    if (optiuni.moddemo == 0) {
        (void)runtime_destroy();
        return EXIT_SUCCESS;
    }

    /* demo-ul real porneste dupa sumar, pentru troubleshooting mai clar la pornire */
    if (ruleazaskeleton(&context) != 0) {
        (void)runtime_destroy();
        return EXIT_FAILURE;
    }

    if (runtime_destroy() != 0) {
        return EXIT_FAILURE;
    }

    if (printf("demo finalizat. rezultatele sunt in directorul %s\n", context.setari.directoroutput) < 0) {
        perror("printf");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/*

exemple build:
./build.sh
gcc -Wall -Wextra -Wpedantic -Werror -g -std=c11 -D_POSIX_C_SOURCE=200809L -pthread server.c unixds.c inetds2.c soapds.c proto.c $(pkg-config --cflags --libs libconfig) -o server


exemple rulare cu eroare:
./server -c config/lipsa.cfg -d


exemple rulare cu succes:
./server -c config/sound_sequencer.cfg -d -e

*/
