/*
 * Cojocaru Vlad
 * IR3 - grupa 4
 * Proiect curs - T32 sound sequencer - client administrare
 * client UNIX simplu pentru operatii de mentenanta
 */

#include <errno.h> /* coduri errno, pentru tratarea erorilor de I/O si socket */
#include <libconfig.h> /* libconfig, pentru citirea fisierului de configurare */
#include <pwd.h> /* getpwuid_r, pentru afisarea utilizatorului curent la -e */
#include <stdio.h> /* printf si perror, pentru output si diagnostic */
#include <stdlib.h> /* EXIT_SUCCESS/EXIT_FAILURE, pentru coduri de iesire */
#include <string.h> /* strlen, strcmp, memcpy, pentru operatii pe stringuri */
#include <sys/socket.h> /* socket/connect, pentru comunicatia cu serverul admin */
#include <sys/un.h> /* sockaddr_un/AF_UNIX, pentru endpoint local UNIX */
#include <unistd.h> /* read/write/close/getuid, pentru I/O si identitate user */

enum {
    /* buffers mici si controlate, pentru comenzi admin scurte */
    marimetext = 128,
    baza10 = 10,
    marimecale = 512,
    marimelinie = 1024
};

struct optiuniadmin {
    char caleconfig[marimecale];
    int listare;
    int afiseazamediu;
};

/* scrie exact tot bufferul, pentru a nu pierde octeti pe socket UNIX */
static int scrietot(int descriptor, const char *text, size_t lungime)
{
    size_t scris;

    scris = 0U;
    while (scris < lungime) {
        ssize_t rezultat;

        rezultat = write(descriptor, text + scris, lungime - scris);
        if (rezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("write");
            return -1;
        }

        scris += (size_t)rezultat;
    }

    return 0;
}

/* helper text, pentru cod mai curat la trimiterea comenzilor */
static int scrietext(int descriptor, const char *text)
{
    return scrietot(descriptor, text, strlen(text));
}

/* copiere defensiva de string, pentru a evita overflow in buffere locale */
static int copiatext(char *destinatie, size_t marime, const char *sursa)
{
    size_t indice;

    indice = 0U;
    while (sursa[indice] != '\0') {
        if (indice + 1U >= marime) {
            return -1;
        }
        destinatie[indice] = sursa[indice];
        indice++;
    }

    destinatie[indice] = '\0';
    return 0;
}

/* conversie int->text fara stdio complex, pentru afisari de eroare controlate */
static int numarlatext(int valoare, char *text, size_t marime)
{
    char invers[marimetext];
    size_t index;
    size_t pozitie;
    unsigned int numar;

    if (marime == 0U) {
        return -1;
    }

    numar = (unsigned int)valoare;
    index = 0U;
    do {
        invers[index] = (char)('0' + (numar % baza10));
        numar /= baza10;
        index++;
    } while (numar != 0U && index < sizeof(invers));

    pozitie = 0U;
    while (index > 0U) {
        if (pozitie + 1U >= marime) {
            return -1;
        }

        index--;
        text[pozitie] = invers[index];
        pozitie++;
    }

    text[pozitie] = '\0';
    return 0;
}

/* citeste utilizatorul curent, pentru afisarea optiunii -e */
static int obtineutilizator(char *user, size_t marime)
{
    char buffer[marimecale];
    struct passwd intrare;
    struct passwd *rezultat;

    if (getpwuid_r(getuid(), &intrare, buffer, sizeof(buffer), &rezultat) == 0 &&
        rezultat != NULL &&
        copiatext(user, marime, rezultat->pw_name) == 0) {
        return 0;
    }

    return copiatext(user, marime, "nedefinit");
}

/* seteaza optiuni implicite, pentru rulare fara argumente suplimentare */
static int initializeazaoptiuni(struct optiuniadmin *optiuni)
{
    if (copiatext(optiuni->caleconfig, sizeof(optiuni->caleconfig), "config/sound_sequencer.cfg") != 0) {
        return -1;
    }

    optiuni->listare = 0;
    optiuni->afiseazamediu = 0;
    return 0;
}

/* parseaza argumentele CLI, pentru controlul modului de rulare */
static int parseazaoptiuni(int argc, char **argv, struct optiuniadmin *optiuni)
{
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "-c") == 0) {
            index++;
            if (index >= argc || copiatext(optiuni->caleconfig, sizeof(optiuni->caleconfig), argv[index]) != 0) {
                return -1;
            }
            continue;
        }

        if (strcmp(argv[index], "-l") == 0) {
            optiuni->listare = 1;
            continue;
        }

        if (strcmp(argv[index], "-e") == 0) {
            optiuni->afiseazamediu = 1;
            continue;
        }

        if (strcmp(argv[index], "-h") == 0) {
            return 1;
        }

        return -1;
    }

    return 0;
}

/* afiseaza eroarea de config detaliat, pentru diagnostic rapid */
static int afiseazaeroareconfig(const config_t *config)
{
    char linie[marimetext];
    const char *fisier;
    const char *text;

    fisier = config_error_file(config);
    text = config_error_text(config);
    if (numarlatext(config_error_line(config), linie, sizeof(linie)) != 0) {
        return -1;
    }

    if (scrietext(STDERR_FILENO, "config: ") != 0 ||
        scrietext(STDERR_FILENO, fisier == NULL ? "necunoscut" : fisier) != 0 ||
        scrietext(STDERR_FILENO, ":") != 0 ||
        scrietext(STDERR_FILENO, linie) != 0 ||
        scrietext(STDERR_FILENO, " - ") != 0 ||
        scrietext(STDERR_FILENO, text == NULL ? "eroare necunoscuta" : text) != 0 ||
        scrietext(STDERR_FILENO, "\n") != 0) {
        return -1;
    }

    return 0;
}

/* textul de help, pentru utilizare rapida din terminal */
static int afiseazaajutor(void)
{
    if (scrietext(STDOUT_FILENO, "folosire: ./admin_client [-c config] [-l] [-e] [-h]\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -c  fisierul de configurare libconfig\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -l  ruleaza toate operatiile admin\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -e  afiseaza si informatii de mediu\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -h  afiseaza acest mesaj\n") != 0) {
        return -1;
    }

    return 0;
}

/* citeste un raspuns de linie, pentru protocolul text admin */
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

/* trimite o comanda admin si citeste raspunsul, pentru schimb request/response sincron */
static int trimitecomanda(int descriptor, const char *comanda, char *raspuns, size_t marimeraspuns)
{
    if (scrietext(descriptor, comanda) != 0 ||
        scrietext(descriptor, "\n") != 0) {
        return -1;
    }

    return citestelinie(descriptor, raspuns, marimeraspuns);
}

/* conectare la socketul UNIX al serverului, pentru canalul de administrare local */
static int conecteazaunix(const char *cale)
{
    struct sockaddr_un adresa;
    int descriptor;

    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        perror("socket");
        return -1;
    }

    memset(&adresa, 0, sizeof(adresa));
    adresa.sun_family = AF_UNIX;
    if (strlen(cale) >= sizeof(adresa.sun_path)) {
        (void)close(descriptor);
        return -1;
    }
    (void)memcpy(adresa.sun_path, cale, strlen(cale) + 1U);

    if (connect(descriptor, (const struct sockaddr *)&adresa, sizeof(adresa)) < 0) {
        perror("connect");
        (void)close(descriptor);
        return -1;
    }

    return descriptor;
}

int main(int argc, char **argv)
{
    config_t config;
    struct optiuniadmin optiuni;
    char socketadmin[marimecale];
    const char *text;
    int timeoutadmin;
    int descriptor;

    /* incarcam valori implicite, pentru pornire rapida */
    if (initializeazaoptiuni(&optiuni) != 0) { 
        return EXIT_FAILURE;
    }

    timeoutadmin = parseazaoptiuni(argc, argv, &optiuni); /* aplicam override-uri CLI */
    if (timeoutadmin < 0) {
        return EXIT_FAILURE;
    }

    if (timeoutadmin > 0) {
        return afiseazaajutor() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    config_init(&config); /* citim socket-ul admin din config, pentru conectare la server */
    if (config_read_file(&config, optiuni.caleconfig) == CONFIG_FALSE) {
        (void)afiseazaeroareconfig(&config);
        config_destroy(&config);
        return EXIT_FAILURE;
    }

    if (config_lookup_string(&config, "server.admin_socket", &text) == CONFIG_FALSE ||
        copiatext(socketadmin, sizeof(socketadmin), text) != 0 ||
        config_lookup_int(&config, "server.admin_timeout", &timeoutadmin) == CONFIG_FALSE) {
        config_destroy(&config);
        (void)scrietext(STDERR_FILENO, "configuratie admin invalida\n");
        return EXIT_FAILURE;
    }

    config_destroy(&config);

    if (printf("client admin\n") < 0 ||
        printf("socket UNIX: %s\n", socketadmin) < 0 ||
        printf("timeout administrare: %d secunde\n", timeoutadmin) < 0) {
        perror("printf");
        return EXIT_FAILURE;
    }

    if (optiuni.afiseazamediu != 0) {
        char user[marimetext];

        if (obtineutilizator(user, sizeof(user)) != 0) {
            return EXIT_FAILURE;
        }

        if (printf("mediu: USER=%s\n", user) < 0) {
            perror("printf");
            return EXIT_FAILURE;
        }
    }

    /* deschidem sesiunea UNIX, pentru comenzi de mentenanta */
    descriptor = conecteazaunix(socketadmin); 
    if (descriptor < 0) {
        return EXIT_FAILURE;
    }

    if (optiuni.listare != 0) {
        /* rulam setul complet de comenzi admin, pentru verificare operationala rapida */
        const char *comenzi[] = {"INFO", "CLIENTS", "JOBS", "QUEUE", "API", "STATS", "HISTORY"};
        size_t index;

        for (index = 0U; index < (sizeof(comenzi) / sizeof(comenzi[0])); index++) {
            char raspuns[marimelinie];

            if (trimitecomanda(descriptor, comenzi[index], raspuns, sizeof(raspuns)) != 0 ||
                printf("%s\n", raspuns) < 0) {
                perror("admin");
                (void)close(descriptor);
                return EXIT_FAILURE;
            }
        }
    } else {
        /* fallback minim pe STATS, pentru executie rapida implicita */
        char raspuns[marimelinie];

        if (trimitecomanda(descriptor, "STATS", raspuns, sizeof(raspuns)) != 0 ||
            printf("%s\n", raspuns) < 0) {
            perror("admin");
            (void)close(descriptor);
            return EXIT_FAILURE;
        }
    }

    (void)trimitecomanda(descriptor, "QUIT", socketadmin, sizeof(socketadmin)); /* inchidem sesiunea protocolului */
    if (close(descriptor) < 0) {
        perror("close");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
