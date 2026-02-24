/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systemes embarques temps reel
 * Hiver 2026
 * Marc-Andre Gardner
 * 
 * Fichier implementant le programme de redimensionnement d'images
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

    char *entree = NULL, *sortie = NULL;
    struct SchedParams schedParams = {0};
    unsigned int largeurSortie = 0, hauteurSortie = 0;
    int modeResize = 0;

    // Lecture des arguments de la ligne de commande
    if(argc < 2){ // On veut au moins le nom du programme et les flux d'entree/sortie
        printf("Nombre d'arguments insuffisant\n");
        return -1;
    }

    if(strcmp(argv[1], "--debug") == 0){ // Mode debug
        printf("Mode debug selectionne pour le redimensionneur\n");
        entree = (char*)"/mem1";
        sortie = (char*)"/mem2";
        largeurSortie = 427;
        hauteurSortie = 240;
        modeResize = 0;
    }
    else{
        int c;
        opterr = 0;
        
        // Lecture des options de la ligne de commande, avec getopt. Les options sont les suivantes :
        // -s : mode d'ordonnancement (NORT, RR, FIFO, DEADLINE)
        // -d : paramètres pour l'ordonnancement deadline (runtime,deadline,period en millisecondes, séparés par des virgules)
        // -w : largeur de sortie
        // -h : hauteur de sortie
        // -r : mode de redimensionnement (0 = plus proche voisin, 1 = bilineaire)
        while((c = getopt(argc, argv, "s:d:w:h:r:")) != -1){ // On lit les options
            switch(c){
                case 's': // Option d'ordonnancement
                    parseSchedOption(optarg, &schedParams);
                    break;
                case 'd': // Option de paramètres pour l'ordonnancement deadline
                    parseDeadlineParams(optarg, &schedParams);
                    break;
                case 'w': //    Largeur de sortie
                    largeurSortie = (unsigned int)strtoul(optarg, NULL, 10);
                    break;
                case 'h': //    Hauteur de sortie
                    hauteurSortie = (unsigned int)strtoul(optarg, NULL, 10);
                    break;
                case 'r': //    Mode de redimensionnement (0 = plus proche voisin, 1 = bilineaire)
                    modeResize = atoi(optarg); // Pas besoin de strtoul ici, on veut juste 0 ou 1
                    break;
                default:
                    continue;
            }
        }

        if(argc - optind < 2){ // On veut au moins les flux d'entree et de sortie, optind est l'index du premier argument non optionnel. 
            printf("Arguments manquants (flux_entree flux_sortie)\n");
            return -1;
        }
        if(largeurSortie == 0 || hauteurSortie == 0){
            printf("Dimensions de sortie invalides (utilisez -w et -h)\n");
            return -1;
        }
        if(modeResize != 0 && modeResize != 1){
            printf("Mode de redimensionnement invalide (utilisez -r 0 ou -r 1)\n");
            return -1;
        }
        // Les flux d'entree et de sortie sont les derniers arguments
        entree = argv[optind];
        sortie = argv[optind+1];
    }

    printf("Initialisation redimensionneur, entree=%s, sortie=%s, largeur=%u, hauteur=%u, mode=%d, ordonnancement=%i\n",
           entree, sortie, largeurSortie, hauteurSortie, modeResize, schedParams.modeOrdonnanceur);

    // Applique les paramètres d'ordonnancement avant d'initialiser les zones de mémoire partagée, pour éviter de potentiels problèmes de permissions
    appliquerOrdonnancement(&schedParams, "redimensionneur");

    // Initialisation des zones de mémoire partagée
    struct memPartage zoneEntree = {0}; // Entrée en tant que lecteur
    if(initMemoirePartageeLecteur(entree, &zoneEntree) != 0){ 
        printf("Erreur d'initialisation de la zone d'entree %s\n", entree);
        return -1;
    }

    // La zone de sortie doit être initialisée AVANT de préparer la mémoire, car elle contient les informations sur la taille des images de sortie, 
    // qui sont nécessaires pour préparer la mémoire
    struct videoInfos infosSortie = {
        .largeur = largeurSortie,
        .hauteur = hauteurSortie,
        .canaux = zoneEntree.header->infos.canaux,
        .fps = zoneEntree.header->infos.fps
    };
    struct memPartage zoneSortie = {0}; // Sortie en tant qu'écrivain
    if(initMemoirePartageeEcrivain(sortie, &zoneSortie, &infosSortie) != 0){
        printf("Erreur d'initialisation de la zone de sortie %s\n", sortie);
        return -1;
    }

    // Préparation de la mémoire pour les images d'entrée et de sortie, en utilisant les tailles indiquées dans les zones de mémoire partagée (pour l'entrée) et
    // les arguments de la ligne de commande (pour la sortie)
    const size_t tailleEntree = (size_t)zoneEntree.header->infos.largeur * (size_t)zoneEntree.header->infos.hauteur * (size_t)zoneEntree.header->infos.canaux;
    const size_t tailleSortie = (size_t)largeurSortie * (size_t)hauteurSortie * (size_t)zoneEntree.header->infos.canaux;

    // Le redimensionnement utilise aussi des grilles (unsigned int/float) potentiellement plus
    // grosses qu'une image 427x240x3. On dimensionne donc les "gros" blocs pour couvrir le pire cas.
    size_t nbElementsGrille = 0;
    size_t tailleElementGrille = (sizeof(unsigned int) > sizeof(float)) ? sizeof(unsigned int) : sizeof(float);
    if (!safe_mul_size((size_t)largeurSortie, (size_t)hauteurSortie, &nbElementsGrille)) {
        printf("Dimensions de sortie invalides (overflow)\n");
        return -1;
    }
    size_t tailleGrille = 0;
    if (!safe_mul_size(nbElementsGrille, tailleElementGrille, &tailleGrille)) {
        printf("Taille de grille invalide (overflow)\n");
        return -1;
    }

    size_t tailleBlocGros = tailleEntree;
    if (tailleSortie > tailleBlocGros) tailleBlocGros = tailleSortie;
    if (tailleGrille > tailleBlocGros) tailleBlocGros = tailleGrille;

    if(prepareMemoire(tailleBlocGros, tailleBlocGros) != 0){ // Allocation des blocs incluant images + grilles
        printf("Erreur d'initialisation de l'allocateur memoire\n");
        return -1;
    }

    // Verrouillage de la mémoire pour éviter les erreurs de segmentation dues à des pages non allouées pendant l'exécution
    // Current verrouille la mémoire déjà allouée, future verrouille toute mémoire qui sera allouée dans le futur 
    if(mlockall(MCL_CURRENT | MCL_FUTURE) != 0){
        perror("mlockall");
    }

    // Allocation d'un buffer pour l'image de sortie, qui sera réutilisé à chaque itération pour éviter les allocations répétées
    unsigned char* imageSortie = (unsigned char*)tempsreel_malloc(tailleSortie);
    if(imageSortie == NULL){
        printf("Erreur d'allocation du buffer de sortie\n");
        return -1;
    }

    // Initialisation de la grille de redimensionnement, qui contient les indices et les poids pour le redimensionnement, 
    // calculés à partir des tailles d'entrée et de sortie
    const unsigned int largeurEntree = zoneEntree.header->infos.largeur;
    const unsigned int hauteurEntree = zoneEntree.header->infos.hauteur;
    const unsigned int canaux = zoneEntree.header->infos.canaux;
    const ResizeGrid rg = (modeResize == 0) // Initialisation de la grille de redimensionnement en fonction du mode choisi 
        ? resizeNearestNeighborInit(hauteurSortie, largeurSortie, hauteurEntree, largeurEntree)
        : resizeBilinearInit(hauteurSortie, largeurSortie, hauteurEntree, largeurEntree);

        // Boucle principale de traitement : on attend une image sur la zone d'entrée, on la redimensionne
        // puis on écrit le résultat sur la zone de sortie
    while(1){
        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXLECTURE); // Profilage : attente de la lecture d'une nouvelle image
        if(attenteLecteur(&zoneEntree) != 0){ // Si la zone d'entrée n'est pas prête à être lue, on recommence la boucle 
            continue;
        }
        // A ce point, on a une nouvelle image à traiter dans zoneEntree.data, de taille tailleEntree. 
        // On peut la redimensionner et la stocker dans imageSortie (notre buffer)
        // qui sera ensuite copiée dans zoneSortie.data pour être envoyée à l'écrivain de la zone de sortie.
        evenementProfilage(&profInfos, ETAT_TRAITEMENT); // Profilage : début du traitement de l'image
        if(modeResize == 0){ // Redimensionnement au plus proche voisin, selon le mode choisi
            resizeNearestNeighbor(zoneEntree.data, hauteurEntree, largeurEntree,
                                  imageSortie, hauteurSortie, largeurSortie, rg, canaux);
        }
        else{
            resizeBilinear(zoneEntree.data, hauteurEntree, largeurEntree,
                           imageSortie, hauteurSortie, largeurSortie, rg, canaux);
        }
        signalLecteur(&zoneEntree); // Signal que la lecture de l'image est terminée, pour que le producteur puisse écrire la prochaine image

        evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE); // Profilage : attente de la possibilité d'écrire la nouvelle image
        if(attenteEcrivain(&zoneSortie) != 0){
            continue;
        }
        // A ce point, on peut écrire l'image redimensionnée dans la zone de sortie pour qu'elle soit traitée par le consommateur de cette zone
        memcpy(zoneSortie.data, imageSortie, tailleSortie); // Copie de l'image redimensionnée dans la zone de sortie
        signalEcrivain(&zoneSortie); // Signal que l'écriture de l'image est terminée, pour que le consommateur puisse lire la nouvelle image
    }

    return 0;
}
