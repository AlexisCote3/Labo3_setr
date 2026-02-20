/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant les fonctions de communication inter-processus
 ******************************************************************************/

#include "commMemoirePartagee.h"
#include "utils.h"

// TODO: implementez ici les fonctions decrites dans commMemoirePartagee.h


int initMemoirePartageeEcrivain(const char* identifiant, struct memPartage *zone, struct videoInfos *infos) {

    if (!identifiant || !zone || !infos) { errno = EINVAL; return -1; }

    memset(zone, 0, sizeof(*zone));

    size_t nb_pixels = 0, nb_octets_total = 0;

    if (!safe_mul_size((size_t)infos->largeur, (size_t)infos->hauteur, &nb_pixels) 
        || !safe_mul_size(nb_pixels, (size_t)infos->canaux, &nb_octets_total) 
        || nb_octets_total == 0) {

            errno = EINVAL;
            return -1;
    }


    zone->tailleDonnees = nb_octets_total;

    size_t total = sizeof(struct memPartageHeader) + zone->tailleDonnees;

    int fd = shm_open(identifiant, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return -1;

    if (ftruncate(fd, (off_t)total) != 0) {

        close(fd);
        shm_unlink(identifiant);
        return -1;
    }

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) {
        
        close(fd);
        shm_unlink(identifiant);
        return -1;
    }

    memset(base, 0, total);

    zone->fd = fd;
    zone->header = (struct memPartageHeader*)base;
    zone->data = (unsigned char*)((unsigned char*)base + sizeof(struct memPartageHeader));

    pthread_mutexattr_t mattr;
    pthread_condxattr_t cattr;

    //init le mutex
    if (pthread_metexattr_init(&mattr) != 0) {
        munmap(base, total);
        close(fd);
        shm_unlink(identifiant);
        errno = EINVAL;
        return -1;
    }

    if (pthread_condattr_init(&cattr) != 0) {
        pthread_mutexattr_destroy(&mattr);
        munmap(base, total); 
        close(fd); 
        shm_unlink(identifiant); 
        errno = EINVAL; 
        return -1;
    }

    //mutex placer dans zone memoire partager et peut etre utiliser par plusieurs processus
    if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&mattr);
        munmap(base, total); 
        close(fd); 
        shm_unlink(identifiant); 
        errno = ENOTSUP; 
        return -1;
    }

    if (pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_condattr_destroy(&cattr);
        pthread_mutexattr_destroy(&mattr);
        munmap(base, total); 
        close(fd);
        shm_unlink(identifiant); 
        errno = ENOTSUP; 
        return -1;
    }

    //active l'heritage de priorite
    if (pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT) != 0) {
        pthread_mutexattr_destroy(&mattr);
        munmap(base, total); 
        close(fd);
        shm_unlink(identifiant);
        errno = ENOTSUP;
        return -1;
    }

    //init mutex et conditons dans la zone partager pour ecrivain et lecteur
    if ( pthread_mutex_init(&zone->header->mutex, &mattr) != 0 
    || pthread_cond_init(&zone->header->condEcrivain, &cattr) != 0 
    || pthread_cond_init(&zone->header->condLecteur, &cattr) != 0) {

        pthread_condattr_destroy(&cattr);
        pthread_mutexattr_destroy(&mattr);
        munmap(base, total); 
        close(fd); 
        shm_unlink(identifiant);
        errno = EINVAL;
        return -1;
    }

    pthread_condattr_destroy(&cattr);
    pthread_mutexattr_destroy(&mattr);

    zone->header->infos = *infos;
    zone->header->etat  = ETAT_PRET_SANS_DONNEES;

    return 0;
}


int initMemoirePartageeLecteur(const char* identifiant, struct memPartage *zone) {

    if (!identifiant || !zone) {
        errno = EINVAL;
        return -1;
    }

    memset(zone, 0, sizeof(*zone));

    //boucle infinie car doit attendre que l'ecrivain init le fichier dans la memoire
    for (;;) {

        fd = shm_open(identifiant, O_RDWR, 0666);
        if (fd >= 0) break;

        if (errno == ENOENT) {
            usleep(DELAI_INIT_READER_USEC);
            continue;
        }

        return -1;
    }


    struct stat st;

    //attendre que la taille soit au moins elle du header
    for (;;) {

        //retourne statistique du fd
        if (fstat(fd, &st) != 0) {
            close(fd);
            return -1;
        }

        if ((size_t)st.st_size >= sizeof(struct memPartageHeader)) break;
        usleep(DELAI_INIT_READER_USEC);
    }

    size_t total = (size_t)st.st_size;
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) {
        close(fd);
        return -1;
    }

    zone->fd = fd;
    zone->header = (struct memPartageHeader*)base;
    zone->tailleDonnees = total - sizeof(struct memPartageHeader);
    zone->data = (unsigned char*)((unsigned char*)base + sizeof(struct memPartageHeader));

    while (zone->header->etat == ETAT_NON_INITIALISE) {
        usleep(DELAI_INIT_READER_USEC);
    }

    return 0;
}


int attenteLecteur(struct memPartage *zone) {

    if (!zone || !zone->header) {
        errno = EINVAL;
        return -1;
    }    

    int rc = pthread_mutex_lock(&zone->header->mutex);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    while (zone->header->etat != ETAT_PRET_AVEC_DONNEES) {

        //relache le mutex et le reprend lorsquil recoit le signal
        rc = pthread_cond_wait(&zone->header->condLecteur, &zone->header->mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&zone->header->mutex);
            errno = rc;
            return -1;
        }
    }

    return 0;
}


int attenteLecteurAsync(struct memPartage *zone) {

    if (!zone || !zone->header) {
        errno = EINVAL;
        return -1;
    } 

    int rc = pthread_mutex_trylock(&zone->header->mutex);
    if (rc != 0) {
        // mutex occupe on ne bloque pas
        return 0;
    }

    if (zone->header->etat == ETAT_PRET_AVEC_DONNEES) {
        // mutex reste verrouille
        return 1;
    }

    pthread_mutex_unlock(&zone->header->mutex);
    return 0;
}

int attenteEcrivain(struct memPartage *zone) {

    if (!zone || !zone->header) {
        errno = EINVAL;
        return -1;
    } 

    int rc = pthread_mutex_lock(&zone->header->mutex);
    if (rc != 0) { 
        errno = rc; 
        return -1;
    }

    while (zone->header->etat != ETAT_PRET_SANS_DONNEES) {
        rc = pthread_cond_wait(&zone->header->condEcrivain, &zone->header->mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&zone->header->mutex);
            errno = rc;
            return -1;
        }
    }

    return 0;
}

void signalLecteur(struct memPartage *zone) {

    if (!zone || !zone->header) return;

    zone->header->etat = ETAT_PRET_SANS_DONNEES;
    pthread_cond_signal(&zone->header->condEcrivain);
    pthread_mutex_unlock(&zone->header->mutex);
}

void signalEcrivain(struct memPartage *zone) {

    if (!zone || !zone->header) return;

    zone->header->etat = ETAT_PRET_AVEC_DONNEES;
    pthread_cond_signal(&zone->header->condLecteur);
    pthread_mutex_unlock(&zone->header->mutex);
}
