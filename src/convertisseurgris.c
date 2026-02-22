/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de conversion en niveaux de gris
 ******************************************************************************/

// Gestion des ressources et permissions
#include <sys/resource.h>


#include "allocateurMemoire.h"
#include "commMemoirePartagee.h"
#include "utils.h"


int main(int argc, char* argv[]){
    // On desactive le buffering pour les printf(), pour qu'il soit possible de les voir depuis votre ordinateur
    setbuf(stdout, NULL);
    
    // Initialise le profilage
    char signatureProfilage[128] = {0};
    char* nomProgramme = (argv[0][0] == '.') ? argv[0]+2 : argv[0];
    snprintf(signatureProfilage, 128, "profilage-%s-%u.txt", nomProgramme, (unsigned int)getpid());
    InfosProfilage profInfos;
    initProfilage(&profInfos, signatureProfilage);
    
    // Premier evenement de profilage : l'initialisation du programme
    evenementProfilage(&profInfos, ETAT_INITIALISATION);
    
    // Code lisant les options sur la ligne de commande
    char *entree, *sortie;                          // Zones memoires d'entree et de sortie
    struct SchedParams schedParams = {0};           // Paramètres de l'ordonnanceur
    unsigned int runtime, deadline, period;         // Dans le cas de l'ordonnanceur DEADLINE

    if(argc < 2){
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){
        // Mode debug, vous pouvez changer ces valeurs pour ce qui convient dans vos tests
        printf("Mode debug selectionne pour le convertisseur niveau de gris\n");
        entree = (char*)"/mem1";
        sortie = (char*)"/mem2";
    }
    else{
        int c;
        opterr = 0;

        while ((c = getopt (argc, argv, "s:d:")) != -1){
            switch (c) {
                case 's':
                    parseSchedOption(optarg, &schedParams);
                    break;
                case 'd':
                    parseDeadlineParams(optarg, &schedParams);
                    break;
                default:
                    continue;
            }
        }

        // Ce qui suit est la description des zones memoires d'entree et de sortie
        if(argc - optind < 2){
            printf("Arguments manquants (fichier_entree flux_sortie)\n");
            return -1;
        }
        entree = argv[optind];
        sortie = argv[optind+1];
    }

    printf("Initialisation convertisseur, entree=%s, sortie=%s, mode d'ordonnancement=%i\n", entree, sortie, schedParams.modeOrdonnanceur);
    
    // Changement de mode d'ordonnancement
    appliquerOrdonnancement(&schedParams, "convertisseur");
    
    // TODO : Écrivez ici le code initialisant les zones mémoire partagées (une en entrée, en tant que lecteur, et l'autre en sortie,
    // en tant qu'écrivain).
    // Initialisez également votre allocateur mémoire (avec prepareMemoire). Assurez-vous que toute la mémoire utilisée dans la
    // section critique est ainsi préallouée ET bloquée (voir documentation de mlock/mlockall).

    struct memPartage zone_in, zone_out;

    if (initMemoirePartageeLecteur(entree, &zone_in) != 0) {
        perror("initMemoirePartageeLecteur");
        return -1;
    }

    struct videoInfos infos_in = zone_in.header->infos;
    if (infos_in.canaux != 3) {
        // Devrait etre BGR (3 canaux)
        fprintf(stderr, "Entree inattendue: canaux=%u\n", infos_in.canaux);
        return -1;
    }

    struct videoInfos infos_out = infos_in;
    //changer a un canal gris
    infos_out.canaux = 1;

    if (initMemoirePartageeEcrivain(sortie, &zone_out, &infos_out) != 0) {
        perror("initMemoirePartageeEcrivain");
        return -1;
    }

    const size_t frame_bytes_in = frame_bytes(&infos_in);
    const size_t frame_bytes_out = frame_bytes(&infos_out);

    // fois 5 et plus (1024 * 1024) pour etre safe
    size_t pool_size = 5 * (frame_bytes_in + frame_bytes_out) + (1024 * 1024);

    if (prepareMemoire(pool_size) != 0) {
        fprintf(stderr, "prepareMemoire a echoue\n");
        return -1;
    }

    //verouiller dans la memoire RAM
    struct rlimit lim;
    lim.rlim_cur = lim.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_MEMLOCK, &lim);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        // pas fatal
    }

    unsigned char* buffer_in = (unsigned char*)tempsreel_malloc(frame_bytes_in);
    unsigned char* buffer_out = (unsigned char*)tempsreel_malloc(frame_bytes_out);
    if (!buffer_in || !buffer_out) {
        fprintf(stderr, "Allocation buffers temps reel echouee\n");
        return -1;
    }


    
    // Section critique (boucle à l'infini).
    while(1){
        // Écrivez le code permettant de convertir une image en niveaux de gris, en utilisant la
        // fonction convertToGray de utils.c. Votre code doit lire une image depuis une zone mémoire 
        // partagée et envoyer le résultat sur une autre zone mémoire partagée.

        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE);
        //prendre le mutex de lecture
        if (attenteLecteur(&zone_in) != 0) {
            continue;
        }

        memcpy(buffer_in, zone_in.data, frame_bytes_in);
        //relacher le mutex
        signalLecteur(&zone_in);

        evenementProfilage(&profInfos, ETAT_TRAITEMENT);
        convertToGray(buffer_in, infos_in.hauteur, infos_in.largeur, infos_in.canaux, buffer_out);

        //Ecriture vers sortie B
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);
        if (attenteEcrivain(&zone_out) != 0) {
            continue;
        }

        memcpy(zone_out.data, buffer_out, frame_bytes_out);
        signalEcrivain(&zone_out);

    }

    return 0;
}
