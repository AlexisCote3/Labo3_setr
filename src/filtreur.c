/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de filtrage des images
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
    char *entree = NULL, *sortie = NULL;
    struct SchedParams schedParams = {0};
    int modeFiltre = 0; // 0 = passe-bas, 1 = passe-haut

    // Lecture des arguments de la ligne de commande
    if(argc < 2){ // On veut au moins le nom du programme et les flux d'entree/sortie
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }
    // Mode debug : on utilise des valeurs par défaut pour les flux d'entrée et de sortie, et le type de filtre
    if(strcmp(argv[1], "--debug") == 0){
        printf("Mode debug selectionné pour le filtreur\n");
        entree = (char*)"/mem1";
        sortie = (char*)"/mem2";
        modeFiltre = 0;
    }
    else{
        // Lecture des options de la ligne de commande, avec getopt. Les options sont les suivantes :
        // -s : mode d'ordonnancement (NORT, RR, FIFO, DEADLINE)
        // -d : paramètres pour l'ordonnancement deadline (runtime,deadline,period en millisecondes, séparés par des virgules)
        // -f : type de filtre (0 = passe-bas, 1 = passe-haut)
        int c;
        opterr = 0; // On veut que getopt ne print pas de message d'erreur pour les options inconnues, on gère ça nous-mêmes
        while((c = getopt(argc, argv, "s:d:f:")) != -1){
            switch(c){
                case 's': // Option d'ordonnancement
                    parseSchedOption(optarg, &schedParams);
                    break;
                case 'd': // Option de paramètres pour l'ordonnancement deadline
                    parseDeadlineParams(optarg, &schedParams);
                    break;
                case 'f': // Type de filtre
                    modeFiltre = atoi(optarg);
                    break;
                default: // Options non reconnues, on les ignore 
                    continue;
            }
        }
         // Ce qui suit est la description des zones memoires d'entree et de sortie
        if(argc - optind < 2){ // On veut au moins les flux d'entree et de sortie, optind est l'index du premier argument non optionnel.
            printf("Arguments manquants (flux_entree flux_sortie)\n");
            return -1;
        }
        // Vérification de la validité de l'argument de filtre
        if(modeFiltre != 0 && modeFiltre != 1){
            printf("Type de filtre invalide (utilisez -f 0 ou -f 1)\n");
            return -1;
        }
        
        // Les flux d'entree et de sortie sont les derniers arguments
        entree = argv[optind];
        sortie = argv[optind + 1];
    }

    printf("Initialisation filtreur, entree=%s, sortie=%s, filtre=%d, mode d'ordonnancement=%i\n",
           entree, sortie, modeFiltre, schedParams.modeOrdonnanceur);

    // Applique les paramètres d'ordonnancement avant d'initialiser les zones de mémoire partagée
    // pour éviter de potentiels problèmes de permissions
    appliquerOrdonnancement(&schedParams, "filtreur");

    // Initialisation de la zone de mémoire partagée d'entrée en tant que lecteur
    struct memPartage zoneEntree = {0};
    if(initMemoirePartageeLecteur(entree, &zoneEntree) != 0){ 
        printf("Erreur d'initialisation de la zone d'entree %s\n", entree);
        return -1;
    }

    // Initialisation de la zone de mémoire partagée de sortie en tant qu'écrivain, avec les mêmes dimensions que la zone d'entrée
    struct videoInfos infosSortie = zoneEntree.header->infos;
    struct memPartage zoneSortie = {0};
    if(initMemoirePartageeEcrivain(sortie, &zoneSortie, &infosSortie) != 0){
        printf("Erreur d'initialisation de la zone de sortie %s\n", sortie);
        return -1;
    }

    // Préparation de la mémoire pour les images d'entrée et de sortie, en utilisant les tailles indiquées dans les zones de mémoire partagée (pour l'entrée) et
    // les arguments de la ligne de commande (pour la sortie)
    const size_t tailleImage = (size_t)zoneEntree.header->infos.largeur *
                               (size_t)zoneEntree.header->infos.hauteur *
                               (size_t)zoneEntree.header->infos.canaux;
    if(prepareMemoire(tailleImage, tailleImage) != 0){ // Allocation de la mémoire pour les images d'entrée et de sortie, mêmes tailles pour les deux (on filtre)
        printf("Erreur d'initialisation de l'allocateur memoire\n");
        return -1;
    }

    // Verrouillage de la mémoire pour éviter les page faults en temps réel
    if(mlockall(MCL_CURRENT | MCL_FUTURE) != 0){ // Current pour la mémoire déjà allouée, future pour les allocations futures
        perror("mlockall");
    }

    // Allocation d'un buffer pour l'image de sortie, qui sera réutilisé à chaque itération pour éviter les allocations répétées
    // Taille du buffer = taille d'une image, qui est la même pour l'entrée et la sortie dans le cas du filtrage
    // On utilise notre allocateur de mémoire temps réel pour cette allocation, pour s'assurer que la mémoire est bien préallouée et bloquée
    unsigned char* imageSortie = (unsigned char*)tempsreel_malloc(tailleImage); // 
    if(imageSortie == NULL){
        printf("Erreur d'allocation du buffer de sortie\n");
        return -1;
    }

    // Boucle principale de traitement : on attend une image sur la zone d'entrée, on la filtre, la met dans le buffer puis on écrit le résultat sur la zone de sortie
    const unsigned int hauteur = zoneEntree.header->infos.hauteur;
    const unsigned int largeur = zoneEntree.header->infos.largeur;
    const unsigned int canaux = zoneEntree.header->infos.canaux;

    while(1){
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE); // Profilage : attente de la lecture d'une nouvelle image
        if(attenteLecteur(&zoneEntree) != 0){
            continue;
        }
        // A ce point, on a une nouvelle image à traiter dans zoneEntree.data, de taille tailleImage. 
        // On peut la filtrer et la stocker dans imageSortie (notre buffer)
        // qui sera ensuite copiée dans zoneSortie.data pour être envoyée à l'écrivain de la zone de sortie.
        evenementProfilage(&profInfos, ETAT_TRAITEMENT);
        if(modeFiltre == 0){ // Filtre passe-bas, selon le mode choisi
            lowpassFilter(hauteur, largeur, zoneEntree.data, imageSortie, 3, 5.0f, canaux);
        }
        else{
            highpassFilter(hauteur, largeur, zoneEntree.data, imageSortie, 3, 5.0f, canaux);
        }
        signalLecteur(&zoneEntree); // Signal que la lecture de l'image est terminée, pour que le producteur puisse écrire la prochaine image

        // Profilage : attente de la possibilité d'écrire la nouvelle image
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);
        if(attenteEcrivain(&zoneSortie) != 0){ // Si la zone de sortie n'est pas prête à être écrite, on recommence la boucle
            continue;
        }
        // A ce point, on peut écrire l'image filtrée dans la zone de sortie pour qu'elle soit traitée par le consommateur de cette zone
        memcpy(zoneSortie.data, imageSortie, tailleImage); // Copie de l'image filtrée dans la zone de sortie
        signalEcrivain(&zoneSortie); // Signal que l'écriture de l'image est terminée, pour que le consommateur puisse lire la nouvelle image
    }

    return 0;
}
