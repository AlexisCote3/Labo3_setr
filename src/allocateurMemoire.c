/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systemes embarques temps reel
 * Hiver 2026
 * Marc-Andre Gardner
 *
 * Fichier implementant les fonctions de l'allocateur memoire temps reel
 ******************************************************************************/

#include "allocateurMemoire.h"

// TODO: Implementez ici votre allocateur memoire utilisant l'interface decrite dans allocateurMemoire.h
// Note : vous devrez utiliser des variables globales pour conserver l'etat des allocations entre les appels.

// Structure d'un bloc libre dans la liste des blocs libres
typedef struct BlocLibre {
    struct BlocLibre* suivant;
} BlocLibre;

// Structure d'entete d'un bloc alloue, qui indique de quel pool il provient (petit ou gros)
typedef struct EnteteBloc {
    unsigned char typePool; /* 1 = petit, 2 = gros */
} EnteteBloc;

// Les pools de mémoire pour les petits et gros blocs, ainsi que les listes de blocs libres pour chaque pool
enum {
    POOL_PETIT = 1,
    POOL_GROS = 2
};

// Variables globales pour les pools de mémoire et les listes de blocs libres
static void* g_poolPetit = NULL; // Pointeur vers le pool de mémoire pour les petits blocs
static void* g_poolGros = NULL; // Pointeur vers le pool de mémoire pour les gros blocs

static size_t g_tailleBlocPetit = 0; // Taille totale d'un bloc dans le pool de petits blocs (entete + charge utile)
static size_t g_tailleBlocGros = 0; // Taille totale d'un bloc dans le pool de gros blocs (entete + charge utile)

static BlocLibre* g_libresPetits = NULL; // Pointeur vers la liste des blocs libres dans le pool de petits blocs
static BlocLibre* g_libresGros = NULL; // Pointeur vers la liste des blocs libres dans le pool de gros blocs

// Fonction pour liberer les pools de memoire en cas d'erreur ou de reinitialisation
static void libererPools(void)
{
    if (g_poolPetit != NULL) {
        free(g_poolPetit);
        g_poolPetit = NULL;
    }
    if (g_poolGros != NULL) {
        free(g_poolGros);
        g_poolGros = NULL;
    }
    g_tailleBlocPetit = 0;
    g_tailleBlocGros = 0;
    g_libresPetits = NULL;
    g_libresGros = NULL;
}

// Fonction d'initialisation de l'allocateur memoire, qui alloue les pools de memoire pour les petits et gros blocs 
// en fonction des tailles d'image d'entree et de sortie
int prepareMemoire(size_t tailleImageEntree, size_t tailleImageSortie)
{
    size_t i; // Variable de boucle pour l'initialisation des blocs
    size_t tailleEntete = sizeof(EnteteBloc); // Taille de l'entete de chaque bloc, qui contient des informations sur le type de pool
    size_t chargePetit = ALLOC_TAILLE_PETIT > sizeof(BlocLibre) ? ALLOC_TAILLE_PETIT : sizeof(BlocLibre); // Charge utile pour les petits blocs, qui doit être au moins suffisante pour contenir un BlocLibre (pour la liste des blocs libres)
    size_t chargeGros = tailleImageEntree > tailleImageSortie ? tailleImageEntree : tailleImageSortie; // Charge utile pour les gros blocs, qui doit être au moins suffisante pour contenir une image d'entrée ou de sortie
    size_t totalPetit; // Taille totale du pool de petits blocs (taille d'un bloc * nombre de blocs)
    size_t totalGros; // Taille totale du pool de gros blocs (taille d'un bloc * nombre de blocs)
    unsigned char* base; // Pointeur de base pour l'initialisation des blocs dans les pools

    libererPools(); // Liberer les pools existants en cas de reinitialisation

    if (chargeGros < sizeof(BlocLibre)) { // S'assurer que la charge utile pour les gros blocs est au moins suffisante pour contenir un BlocLibre, pour la gestion de la liste des blocs libres
        chargeGros = sizeof(BlocLibre);
    }

    g_tailleBlocPetit = tailleEntete + chargePetit; // Taille totale d'un bloc dans le pool de petits blocs (entete + charge utile)
    g_tailleBlocGros = tailleEntete + chargeGros; // Taille totale d'un bloc dans le pool de gros blocs (entete + charge utile)

    if (ALLOC_N_PETITS_BLOCS != 0 && g_tailleBlocPetit > ((size_t)-1) / ALLOC_N_PETITS_BLOCS) { // Vérification de débordement pour le calcul de la taille totale du pool de petits blocs
        libererPools();
        return -1;
    }
    if (ALLOC_N_GROS_BLOCS != 0 && g_tailleBlocGros > ((size_t)-1) / ALLOC_N_GROS_BLOCS) { // Vérification de débordement pour le calcul de la taille totale du pool de gros blocs
        libererPools();
        return -1;
    }

    totalPetit = g_tailleBlocPetit * ALLOC_N_PETITS_BLOCS; // Calcul de la taille totale du pool de petits blocs
    totalGros = g_tailleBlocGros * ALLOC_N_GROS_BLOCS; // Calcul de la taille totale du pool de gros blocs

    g_poolPetit = malloc(totalPetit); // Allocation du pool de mémoire pour les petits blocs
    if (g_poolPetit == NULL) {
        libererPools();
        return -1;
    }

    g_poolGros = malloc(totalGros); // Allocation du pool de mémoire pour les gros blocs
    if (g_poolGros == NULL) {
        libererPools();
        return -1;
    }

    // Initialisation des listes de blocs libres pour les petits et gros blocs, en parcourant les pools et en créant les entetes et les blocs libres pour chaque bloc dans les pools
    g_libresPetits = NULL;
    base = (unsigned char*)g_poolPetit;
    for (i = 0; i < ALLOC_N_PETITS_BLOCS; ++i) {
        EnteteBloc* entete = (EnteteBloc*)(base + i * g_tailleBlocPetit);
        BlocLibre* bloc = (BlocLibre*)(entete + 1);
        entete->typePool = POOL_PETIT;
        bloc->suivant = g_libresPetits;
        g_libresPetits = bloc;
    }
    
    // Initialisation de la liste de blocs libres pour les gros blocs
    g_libresGros = NULL;
    base = (unsigned char*)g_poolGros;
    for (i = 0; i < ALLOC_N_GROS_BLOCS; ++i) {
        EnteteBloc* entete = (EnteteBloc*)(base + i * g_tailleBlocGros);
        BlocLibre* bloc = (BlocLibre*)(entete + 1);
        entete->typePool = POOL_GROS;
        bloc->suivant = g_libresGros;
        g_libresGros = bloc;
    }

    return 0;
}

// Fonction d'allocation de memoire temps reel, qui retourne un bloc de memoire de taille au moins egale a la taille demandee, 
// ou NULL en cas d'erreur (par exemple si aucun bloc libre n'est disponible)
void* tempsreel_malloc(size_t taille)
{
    BlocLibre* bloc;
    // On determine le pool a utiliser en fonction de la taille demandee, et on retourne un bloc de ce pool si disponible
    if (taille <= ALLOC_TAILLE_PETIT) {
        if (g_libresPetits == NULL) {
            return NULL;
        }
        bloc = g_libresPetits;
        g_libresPetits = bloc->suivant;
        return (void*)bloc;
    }
    // Si la taille demandee est superieure a ALLOC_TAILLE_PETIT, on utilise le pool de gros blocs
    if (g_libresGros == NULL) {
        return NULL;
    }
    bloc = g_libresGros;
    g_libresGros = bloc->suivant;
    return (void*)bloc;
}

void tempsreel_free(void* ptr)
{
    // On determine le pool auquel appartient le bloc a liberer en lisant l'entete du bloc, puis on le remet dans la liste des blocs libres de ce pool
    EnteteBloc* entete;
    BlocLibre* bloc;
     // Si le pointeur est NULL, on ne fait rien 
    if (ptr == NULL) {
        return;
    }
    // On lit l'entete du bloc pour determiner le pool auquel il appartient, puis on le remet dans la liste des blocs libres de ce pool
    bloc = (BlocLibre*)ptr;
    entete = ((EnteteBloc*)ptr) - 1;
    // On remet le bloc dans la liste des blocs libres du pool correspondant
    if (entete->typePool == POOL_PETIT) {
        bloc->suivant = g_libresPetits;
        g_libresPetits = bloc;
    } else if (entete->typePool == POOL_GROS) {
        bloc->suivant = g_libresGros;
        g_libresGros = bloc;
    }
}

