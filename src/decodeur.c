/******************************************************************************
 * Laboratoire 3
 * GIF-3004 Systèmes embarqués temps réel
 * Hiver 2026
 * Marc-André Gardner
 * 
 * Fichier implémentant le programme de décodage des fichiers ULV
 ******************************************************************************/


// Gestion des ressources et permissions
#include <sys/resource.h>
#include <unistd.h>
#include <getopt.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <time.h>

#include "allocateurMemoire.h"
#include "commMemoirePartagee.h"
#include "utils.h"

#include "jpgd.h"

// Définition de diverses structures pouvant vous être utiles pour la lecture d'un fichier ULV
#define HEADER_SIZE 4
const char header[] = "SETR";
#define ULV_HEADER_BYTES 20  // 4 + 4*4



//Existe deja dans commMemoirePartagee
// struct videoInfos{
//         uint32_t largeur;
//         uint32_t hauteur;
//         uint32_t canaux;
//         uint32_t fps;
// };

/******************************************************************************
* FORMAT DU FICHIER VIDEO
* Offset     Taille     Type      Description
* 0          4          char      Header (toujours "SETR" en ASCII)
* 4          4          uint32    Largeur des images du vidéo
* 8          4          uint32    Hauteur des images du vidéo
* 12         4          uint32    Nombre de canaux dans les images
* 16         4          uint32    Nombre d'images par seconde (FPS)
* 20         4          uint32    Taille (en octets) de la première image -> N
* 24         N          char      Contenu de la première image (row-first)
* 24+N       4          uint32    Taille (en octets) de la seconde image -> N2
* 24+N+4     N2         char      Contenu de la seconde image
* 24+N+N2    4          uint32    Taille (en octets) de la troisième image -> N2
* ...                             Toutes les images composant la vidéo, à la suite
*            4          uint32    0 (indique la fin du fichier)
******************************************************************************/

static int read_u32(const uint8_t* buffer, size_t buffer_size, size_t offset, uint32_t* out) {

    if (offset + 4 > buffer_size) return -1;
    uint32_t value;
    memcpy(&value, buffer + offset, 4);
    *out = value;
    return 0;

}



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
    
    // Écrivez le code de décodage depuis un fichier et d'envoi sur la zone mémoire partagée ici!
    // N'oubliez pas que vous pouvez utiliser jpgd::decompress_jpeg_image_from_memory()
    // pour décoder une image JPEG contenue dans un buffer!
    // N'oubliez pas également que ce décodeur doit lire les fichiers ULV EN BOUCLE

    struct SchedParams params;
    memset(&params, 0, sizeof(params));
    params.modeOrdonnanceur = ORDONNANCEMENT_NORT; //par defaut

    const char* fichier_entree = NULL;
    const char* flux_sortie = NULL;

    if (argc == 2 && strcmp(argv[1], "--debug") == 0) {

        //todo modifier aux bons paths
        fichier_entree = "video.ulv";
        flux_sortie = "/flux1_mem";
    } else {

        int opt;
        while ((opt = getopt(argc, argv, "s:d:")) != -1) {

            switch (opt) {

                case 's':
                    parseSchedOption(optarg, &params);
                    break;

                case 'd':
                    parseDeadlineParams(optarg, &params);
                    break;

                default:
                    break;
            }
        }

        if (optind + 2 > argc) {
            fprintf(stderr, "Usage: %s [options] fichier_entree flux_sortie\n", argv[0]);
            fprintf(stderr, "  options: -s {NORT|RR|FIFO|DEADLINE}  -d runtime,deadline,period (ms)\n");
            return 1;
        }

        fichier_entree = argv[optind];
        flux_sortie    = argv[optind + 1];
    }

    if (appliquerOrdonnancement(&params, nomProgramme) != 0) {
            fprintf(stderr, "[%s] appliquerOrdonnancement() a échoué: %s\n", nomProgramme, strerror(errno));
            return 1;
        }

        int fd = open(fichier_entree, O_RDONLY);
        if (fd < 0) {perror("open"); return -1;}

        struct stat st;
        if (fstat(fd, &st) != 0) {

            perror("fstat");
            close(fd);
            return 1;
        }

        size_t file_size = (size_t)st.st_size;
        if (file_size < ULV_HEADER_BYTES) {
            fprintf(stderr, "Fichier ULV trop petit\n");
            close(fd);
            return 1;
        }

        //Lire le header pour verifier SETR
        uint8_t ulv_hdr[ULV_HEADER_BYTES];
        ssize_t r = pread(fd, ulv_hdr, ULV_HEADER_BYTES, 0);
        if (r != ULV_HEADER_BYTES) {
            perror("pread");
            close(fd);
            return 1;
        }

        if (memcmp(ulv_hdr, header, 4) != 0) {
            fprintf(stderr, "ULV invalide: header != SETR\n");
            close(fd);
            return 1;
        }

        uint32_t width=0,height=0,channels=0,fps=0;
        if (read_u32(ulv_hdr, ULV_HEADER_BYTES, 4, &width) != 0 ||
            read_u32(ulv_hdr, ULV_HEADER_BYTES, 8, &height) != 0 ||
            read_u32(ulv_hdr, ULV_HEADER_BYTES, 12, &channels) != 0 ||
            read_u32(ulv_hdr, ULV_HEADER_BYTES, 16, &fps) != 0
        ) {
            fprintf(stderr, "ULV invalide (infos)\n");
            close(fd);
            return 1;
        }

        if (!(channels == 1 || channels == 3)) {
            fprintf(stderr, "ULV invalide: channels=%u (attendu 1 ou 3)\n", channels);
            close(fd);
            return 1;
        }

        size_t frame_size = (size_t)width * (size_t)height * (size_t)channels;
        if (frame_size == 0) {
            fprintf(stderr, "ULV invalide (frame_size=0)\n");
            close(fd);
            return 1;
        }



        //----
        //Init memoire partager
        //----

        struct memPartage zoneOut;
        struct videoInfos infos;
        infos.largeur = width;
        infos.hauteur = height;
        infos.canaux  = channels;
        infos.fps     = fps;

        if (initMemoirePartageeEcrivain(flux_sortie, &zoneOut, &infos) != 0) {
            perror("initMemoirePartageeEcrivain");
            close(fd);
            return 1;
        }

        frame_size = zoneOut.tailleDonnees;

        if (prepareMemoire(frame_size, frame_size) != 0) {
            fprintf(stderr, "prepareMemoire(%zu,%zu) a echoue\n", frame_size, frame_size);
            close(fd);
            return 1;
        }

        struct rlimit lim;
        lim.rlim_cur = lim.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
            perror("setrlimit(RLIMIT_MEMLOCK)");
        }
        if (mlockall(MCL_CURRENT) != 0) {
            perror("mlockall(MCL_CURRENT)");
            // pas fatal
        }

        uint8_t* file = (uint8_t*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        close(fd);
        if (file == MAP_FAILED) {
            perror("mmap");
            return 1;
        }

        //periode selon le fps qui est set
        const uint64_t frame_period_us = (fps == 0) ? 0 : (1000000UL/ (uint64_t)fps);

        //----
        //Boucle ULV
        //----
        size_t offset = ULV_HEADER_BYTES;

        while (1) {

            uint64_t t0 = now_us();

            uint32_t jpegSize = 0;

            if (read_u32(file, file_size, offset, &jpegSize) != 0) {
                offset = ULV_HEADER_BYTES;
                continue;
            }

            offset += 4;

            if (jpegSize == 0) {
                // fin recommencer au debut
                offset = ULV_HEADER_BYTES;
                continue;
            }

            if (offset + (size_t)jpegSize > file_size) {
                //corruption on recommence
                offset = ULV_HEADER_BYTES;
                continue;
            }

            //pointeur sur les donnee du frame jpeg
            const unsigned char* jpegBuffer = (const unsigned char*)(file + offset);
            //avancer offset au prochain frame
            offset += (size_t)jpegSize;

            evenementProfilage(&profInfos, ETAT_TRAITEMENT);

            //decompresser le jpeg en image
            int out_width = 0, out_height = 0, out_color = 0;
            unsigned char* decoded = jpgd::decompress_jpeg_image_from_memory(jpegBuffer, (int)jpegSize, &out_width, &out_height, &out_color, (int)channels, 0);

            if (!decoded) {
                continue;
            }

            if ((uint32_t)out_width != width || (uint32_t)out_height != height) {
                tempsreel_free(decoded);
                continue;
            }

            evenementProfilage(&profInfos, ETAT_ATTENTE_MUTEXECRITURE);

            //ecrivain prend le mutex
            if (attenteEcrivain(&zoneOut) != 0) {
                tempsreel_free(decoded);
                continue;
            }

            memcpy(zoneOut.data, decoded, frame_size);
            //signaler terminer decrire.
            signalEcrivain(&zoneOut);

            tempsreel_free(decoded);

            if (frame_period_us > 0) {
                uint64_t t1 = now_us();
                uint64_t elapsed = t1 - t0;
                if (elapsed < frame_period_us) {
                    evenementProfilage(&profInfos, ETAT_ENPAUSE);
                    usleep((useconds_t)(frame_period_us - elapsed));
                }
            }

        }

        
    return 0;
}
