/*
 * Cojocaru Vlad
 * IR3 - grupa 4
 * Proiect curs - T32 sound sequencer - client ordinar C
 * client INET sincron pentru UPLOAD/CHUNK/STATUS/RESULT
 */

#include <arpa/inet.h> /* inet_pton/htons, pentru conectare IPv4 la server */
#include <errno.h> /* errno/EINTR, pentru tratamentul robust al erorilor I/O */
#include <fcntl.h> /* open, pentru citirea fisierului local de upload */
#include <libconfig.h> /* libconfig, pentru citirea portului si compozitiei implicite */
#include <netinet/in.h> /* sockaddr_in, pentru endpoint TCP client */
#include <pwd.h> /* getpwuid_r, pentru afisarea utilizatorului la -e */
#include <stdio.h> /* printf/perror/snprintf, pentru output si protocol text */
#include <stdlib.h> /* EXIT_* si utilitare standard, pentru controlul iesirii */
#include <string.h> /* strcmp/strncmp/memset/sscanf, pentru parsing comenzi */
#include <sys/socket.h> /* socket/connect, pentru canalul INET cu serverul */
#include <sys/stat.h> /* stat, pentru dimensiunea fisierului de upload */
#include <unistd.h> /* read/write/close/sleep/getuid, pentru I/O si polling */

enum {
    /* limite locale de protocol si I/O, pentru transfer stabil fara alocari necontrolate */
    marimetext = 128,
    baza10 = 10,
    marimecale = 512,
    marimelinie = 1024,
    chunk_size = 65536,
    status_poll_seconds = 1,
    status_poll_max_attempts = 600,
    result_retry_attempts = 8
};

struct optiuniclient {
    char caleconfig[marimecale];
    char caleinput[marimecale];
    char numecompozitie[marimetext];
    int arataprotocol;
    int afiseazamediu;
    int areinput;
};

/* scrie tot bufferul in socket/fisier, pentru a evita transfer partial */
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
            perror("write");
            return -1;
        }

        scris += (size_t)rezultat;
    }

    return 0;
}

/* helper text, pentru comenzi protocol trimise ca linii */
static int scrietext(int descriptor, const char *text)
{
    return scrietot(descriptor, text, strlen(text));
}

/* citeste exact numarul cerut de bytes, pentru payload binar complet */
static int citestetot(int descriptor, void *buffer, size_t lungime)
{
    size_t citit;

    citit = 0U;
    while (citit < lungime) {
        ssize_t rezultat;

        rezultat = read(descriptor, (char *)buffer + citit, lungime - citit);
        if (rezultat < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            return -1;
        }

        if (rezultat == 0) {
            return -1;
        }

        citit += (size_t)rezultat;
    }

    return 0;
}

/* citeste o linie text din socket, pentru raspunsurile serverului */
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

/* copiere string cu verificare, pentru protectie la overflow */
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

/* conversie numerica simpla, pentru mesaje text controlate */
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

/* citeste userul curent, pentru afisare in modul -e */
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

/* afiseaza eroarea libconfig detaliat, pentru depanare rapida */
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

/* initializeaza optiuni implicite, pentru rulare simpla fara argumente multe */
static int initializeazaoptiuni(struct optiuniclient *optiuni)
{
    if (copiatext(optiuni->caleconfig, sizeof(optiuni->caleconfig), "config/sound_sequencer.cfg") != 0 ||
        copiatext(optiuni->numecompozitie, sizeof(optiuni->numecompozitie), "demo_string_layers") != 0) {
        return -1;
    }

    optiuni->caleinput[0] = '\0';
    optiuni->arataprotocol = 0;
    optiuni->afiseazamediu = 0;
    optiuni->areinput = 0;
    return 0;
}

/* parseaza argumentele, pentru control explicit al upload-ului */
static int parseazaoptiuni(int argc, char **argv, struct optiuniclient *optiuni)
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

        if (strcmp(argv[index], "-i") == 0) {
            index++;
            if (index >= argc || copiatext(optiuni->caleinput, sizeof(optiuni->caleinput), argv[index]) != 0) {
                return -1;
            }
            optiuni->areinput = 1;
            continue;
        }

        if (strcmp(argv[index], "-n") == 0) {
            index++;
            if (index >= argc || copiatext(optiuni->numecompozitie, sizeof(optiuni->numecompozitie), argv[index]) != 0) {
                return -1;
            }
            continue;
        }

        if (strcmp(argv[index], "-s") == 0) {
            optiuni->arataprotocol = 1;
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

/* afiseaza sumarul runtime, pentru confirmarea destinatiei si compozitiei */
static int afiseazasumar(const char *numecompozitie, const char *numeconfig, int portclienti)
{
    if (printf("client ordinar\n") < 0 ||
        printf("destinatie INET: 127.0.0.1:%d\n", portclienti) < 0 ||
        printf("compozitie ceruta: %s\n", numecompozitie) < 0 ||
        printf("compozitie implicita config: %s\n", numeconfig) < 0) {
        perror("printf");
        return -1;
    }

    return 0;
}

/* afiseaza mediu, pentru debugging in evaluare/laborator */
static int afiseazamediu(void)
{
    char user[marimetext];

    if (obtineutilizator(user, sizeof(user)) != 0) {
        return -1;
    }

    if (printf("mediu: USER=%s\n", user) < 0) {
        perror("printf");
        return -1;
    }

    return 0;
}

/* afiseaza protocolul activ, pentru claritate la demo */
static int afiseazaprotocol(void)
{
    if (printf("protocol sincron:\n") < 0 ||
        printf("UPLOAD <composition_name> <total_bytes>\n") < 0 ||
        printf("CHUNK <index> <chunk_bytes>\n") < 0 ||
        printf("STATUS <composition_name>\n") < 0 ||
        printf("RESULT <composition_name>\n") < 0) {
        perror("printf");
        return -1;
    }

    return 0;
}

/* help CLI, pentru utilizare rapida */
static int afiseazaajutor(void)
{
    if (scrietext(STDOUT_FILENO, "folosire: ./ordinary_client [-c config] [-i fisier] [-n nume] [-s] [-e] [-h]\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -c  fisierul de configurare libconfig\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -i  fisierul de intrare trimis prin upload\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -n  numele compozitiei\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -s  afiseaza protocolul curent\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -e  afiseaza si informatii de mediu\n") != 0 ||
        scrietext(STDOUT_FILENO, "  -h  afiseaza acest mesaj\n") != 0) {
        return -1;
    }

    return 0;
}

/* conecteaza la serverul INET local, pentru canalul de transfer remote */
static int conecteazainet(int port)
{
    struct sockaddr_in adresa;
    int descriptor;

    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        perror("socket");
        return -1;
    }

    memset(&adresa, 0, sizeof(adresa));
    adresa.sin_family = AF_INET;
    adresa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, "127.0.0.1", &adresa.sin_addr) != 1) {
        (void)close(descriptor);
        return -1;
    }

    if (connect(descriptor, (const struct sockaddr *)&adresa, sizeof(adresa)) < 0) {
        perror("connect");
        (void)close(descriptor);
        return -1;
    }

    return descriptor;
}

/* request/response sincron pe o comanda text, pentru interactiune predictibila */
static int trimitecomanda(int descriptor, const char *comanda, char *raspuns, size_t marimeraspuns)
{
    if (scrietext(descriptor, comanda) != 0 || scrietext(descriptor, "\n") != 0) {
        return -1;
    }

    return citestelinie(descriptor, raspuns, marimeraspuns);
}

/* descarca fisierul rezultat de la server, pentru etapa RESULT */
static int descarcarezultat(int descriptor, const char *compozitie)
{
    char comanda[marimelinie];
    char raspuns[marimelinie];
    char caleiesire[marimecale];
    int descriptoriesire;
    size_t marime;

    if (snprintf(comanda, sizeof(comanda), "RESULT %s", compozitie) < 0) {
        return -1;
    }

    if (trimitecomanda(descriptor, comanda, raspuns, sizeof(raspuns)) != 0) {
        return -1;
    }

    if (sscanf(raspuns, "RESULT %zu", &marime) != 1) {
        if (strncmp(raspuns, "ERR RESULT_NOT_READY", 20) == 0) {
            return 1;
        }
        if (printf("%s\n", raspuns) < 0) {
            perror("printf");
            return -1;
        }
        return -1;
    }

    if (snprintf(caleiesire, sizeof(caleiesire), "output/%s_client_download.wav", compozitie) < 0) {
        return -1;
    }

    descriptoriesire = open(caleiesire, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (descriptoriesire < 0) {
        perror("open");
        return -1;
    }

    while (marime > 0U) {
        unsigned char buffer[chunk_size];
        size_t bucata;

        bucata = marime > sizeof(buffer) ? sizeof(buffer) : marime;
        if (citestetot(descriptor, buffer, bucata) != 0 ||
            scrietot(descriptoriesire, buffer, bucata) != 0) {
            (void)close(descriptoriesire);
            return -1;
        }

        marime -= bucata;
    }

    if (close(descriptoriesire) < 0) {
        perror("close");
        return -1;
    }

    if (printf("rezultat salvat in %s\n", caleiesire) < 0) {
        perror("printf");
        return -1;
    }

    return 0;
}

/* sondeaza periodic statusul, pentru a astepta finalizarea procesarii */
static int asteaptarezultatulready(int descriptor, const char *compozitie)
{
    char comanda[marimelinie];
    char raspuns[marimelinie];
    int incercare;

    for (incercare = 0; incercare < status_poll_max_attempts; incercare++) {
        char stare[marimetext];
        size_t primiti;
        size_t total;

        if (snprintf(comanda, sizeof(comanda), "STATUS %s", compozitie) < 0 ||
            trimitecomanda(descriptor, comanda, raspuns, sizeof(raspuns)) != 0) {
            return -1;
        }

        if (sscanf(raspuns, "OK STATUS %127s %zu %zu", stare, &primiti, &total) != 3) {
            if (printf("%s\n", raspuns) < 0) {
                perror("printf");
            }
            return -1;
        }

        if (printf("%s\n", raspuns) < 0) {
            perror("printf");
            return -1;
        }

        if (strcmp(stare, "READY") == 0) {
            return 0;
        }

        if (strcmp(stare, "ERROR") == 0 || strcmp(stare, "NOT_FOUND") == 0) {
            return -1;
        }

        sleep(status_poll_seconds);
    }

    return -1;
}

/* trimite fisierul local in CHUNK-uri, pentru upload incremental controlat */
static int incarcafisier(int descriptor, const char *compozitie, const char *caleinput) /* NOLINT(bugprone-easily-swappable-parameters) */
{
    char comanda[marimelinie];
    char raspuns[marimelinie];
    struct stat stare;
    int descriptorinput;
    size_t indexchunk;

    if (stat(caleinput, &stare) < 0) {
        perror("stat");
        return -1;
    }

    if (stare.st_size < 0) {
        return -1;
    }

    if (printf("fisier upload: %s (%ld bytes)\n", caleinput, (long)stare.st_size) < 0) {
        perror("printf");
        return -1;
    }

    if (snprintf(comanda, sizeof(comanda), "UPLOAD %s %ld", compozitie, (long)stare.st_size) < 0 ||
        trimitecomanda(descriptor, comanda, raspuns, sizeof(raspuns)) != 0) {
        return -1;
    }

    if (strncmp(raspuns, "OK ", 3) != 0) {
        if (printf("%s\n", raspuns) < 0) {
            perror("printf");
            return -1;
        }
        return -1;
    }

    descriptorinput = open(caleinput, O_RDONLY);
    if (descriptorinput < 0) {
        perror("open");
        return -1;
    }

    indexchunk = 0U;
    while (1) {
        unsigned char buffer[chunk_size];
        ssize_t citit;

        citit = read(descriptorinput, buffer, sizeof(buffer));
        if (citit < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            (void)close(descriptorinput);
            return -1;
        }

        if (citit == 0) {
            break;
        }

        if (snprintf(comanda, sizeof(comanda), "CHUNK %zu %zd", indexchunk, citit) < 0 ||
            scrietext(descriptor, comanda) != 0 ||
            scrietext(descriptor, "\n") != 0 ||
            scrietot(descriptor, buffer, (size_t)citit) != 0 ||
            citestelinie(descriptor, raspuns, sizeof(raspuns)) != 0) {
            (void)close(descriptorinput);
            return -1;
        }

        if (strncmp(raspuns, "OK ", 3) != 0) {
            if (printf("%s\n", raspuns) < 0) {
                perror("printf");
                (void)close(descriptorinput);
                return -1;
            }
            (void)close(descriptorinput);
            return -1;
        }

        indexchunk++;
    }

    if (close(descriptorinput) < 0) {
        perror("close");
        return -1;
    }

    if (asteaptarezultatulready(descriptor, compozitie) != 0) {
        return -1;
    }

    for (indexchunk = 0U; indexchunk < result_retry_attempts; indexchunk++) {
        int rezultat;

        /* retry scurt pe RESULT_NOT_READY, pentru toleranta la intarzieri minore de worker */
        rezultat = descarcarezultat(descriptor, compozitie);
        if (rezultat == 0) {
            return 0;
        }
        if (rezultat < 0) {
            return -1;
        }

        sleep(status_poll_seconds);
    }

    return -1;
}

int main(int argc, char **argv)
{
    config_t config;
    struct optiuniclient optiuni;
    char numeconfig[marimetext];
    const char *text;
    int portclienti;
    int descriptor;

    if (initializeazaoptiuni(&optiuni) != 0) {
        return EXIT_FAILURE;
    }

    portclienti = parseazaoptiuni(argc, argv, &optiuni);
    if (portclienti < 0) {
        return EXIT_FAILURE;
    }

    if (portclienti > 0) {
        return afiseazaajutor() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    config_init(&config);
    if (config_read_file(&config, optiuni.caleconfig) == CONFIG_FALSE) {
        (void)afiseazaeroareconfig(&config);
        config_destroy(&config);
        return EXIT_FAILURE;
    }

    if (config_lookup_string(&config, "audio.composition_name", &text) == CONFIG_FALSE ||
        copiatext(numeconfig, sizeof(numeconfig), text) != 0 ||
        config_lookup_int(&config, "server.ordinary_port", &portclienti) == CONFIG_FALSE) {
        config_destroy(&config);
        (void)scrietext(STDERR_FILENO, "configuratie client invalida\n");
        return EXIT_FAILURE;
    }

    config_destroy(&config);

    if (afiseazasumar(optiuni.numecompozitie, numeconfig, portclienti) != 0) {
        return EXIT_FAILURE;
    }

    if (optiuni.afiseazamediu != 0 && afiseazamediu() != 0) {
        return EXIT_FAILURE;
    }

    if (optiuni.arataprotocol != 0 && afiseazaprotocol() != 0) {
        return EXIT_FAILURE;
    }

    if (optiuni.areinput == 0) {
        return EXIT_SUCCESS;
    }

    descriptor = conecteazainet(portclienti);
    if (descriptor < 0) {
        return EXIT_FAILURE;
    }

    if (incarcafisier(descriptor, optiuni.numecompozitie, optiuni.caleinput) != 0) {
        (void)close(descriptor);
        return EXIT_FAILURE;
    }

    (void)trimitecomanda(descriptor, "QUIT", numeconfig, sizeof(numeconfig));
    if (close(descriptor) < 0) {
        perror("close");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
