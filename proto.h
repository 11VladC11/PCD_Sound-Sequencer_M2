#ifndef PROTO_H
#define PROTO_H

/* constante comune de dimensiune, pentru buffere consistente in tot proiectul */
enum {
    marimetext = 128,
    marimenote = 256,
    marimecale = 512,
    marimestraturi = 8
};

/* setari globale server, pentru configurarea endpoint-urilor si a demo-ului audio */
struct setariserver {
    char titlu[marimetext];
    char numecompozitie[marimetext];
    char socketsadmin[marimecale];
    char directoroutput[marimecale];
    int portclienti;
    int portapi;
    int timeoutadmin;
    int samplerate;
};

/* un strat audio din config, pentru procesare/mixaj pe instrumente */
struct strat {
    char instrument[marimetext];
    char note[marimenote];
    char efect[marimetext];
    char caleinput[marimecale];
    int workloadms;
};

/* optiuni CLI pentru executabilul server, pentru controlul modului de rulare */
struct optiuniserver {
    char caleconfig[marimecale];
    int moddemo;
    int afiseazamediu;
};

/* contextul unificat al serverului, pentru a fi trimis catre thread-urile componente */
struct contextserver {
    struct setariserver setari;
    struct strat straturi[marimestraturi];
    int numarstraturi;
};

/* API utilitare server, pentru init/config/afisare/demo */
int initializeazaoptiuni(struct optiuniserver *optiuni);
int parseazaoptiuni(int argc, char **argv, struct optiuniserver *optiuni);
int afiseazaajutor(void);
int createdirector(const char *cale);
int incarca_context(const char *caleconfig, struct contextserver *context);
int afiseazasumar(const struct contextserver *context);
int afiseazamediu(void);
int ruleazademo(const struct contextserver *context);

/* entry points de thread, pentru canalele UNIX/INET/API */
void *unix_main(void *args);
void *inet_main(void *args);
void *soap_main(void *args);

#endif
