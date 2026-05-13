/*
 * Cojocaru Vlad
 * IR3 - grupa 4
 * Proiect curs - T32 sound sequencer - soapds
 * implementeaza componenta ws api pe fir separat pentru portul web planificat
 * afiseaza sumarul portului api folosit in arhitectura curenta a serverului
 * trateaza erori simple de afisare pentru componenta ws api
 */

#include <stdio.h> /* printf si perror la afisare, pentru observabilitate minima */

#include "proto.h" /* contextserver, pentru acces la portul API din configuratie */

void *soap_main(void *args)
{
    const struct contextserver *context;

    context = (const struct contextserver *)args;
    /* aici anunt doar portul api folosit in arhitectura curenta, pentru pregatirea etapei urmatoare */
    if (printf("fir api/ws: port %d dptru etapa urmatoare\n",
               context->setari.portapi) < 0) {
        perror("printf");
        return (void *)1;
    }

    return NULL;
}

/*

exemple build:
./build.sh


exemple rulare cu eroare:
./server -c config/lipsa.cfg -d


exemple rulare cu succes:
./server -c config/sound_sequencer.cfg -d -e


*/
