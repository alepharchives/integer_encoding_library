/*-----------------------------------------------------------------------------
 *  encoders.cpp - Encoders implementing many types of copression algorithms
 *
 *  Coding-Style:
 *      emacs) Mode: C, tab-width: 8, c-basic-offset: 8, indent-tabs-mode: nil
 *      vi) tabstop: 8, expandtab
 *
 *  Authors:
 *      Takeshi Yamamuro <linguin.m.s_at_gmail.com>
 *      Fabrizio Silvestri <fabrizio.silvestri_at_isti.cnr.it>
 *      Rossano Venturini <rossano.venturini_at_isti.cnr.it>
 *-----------------------------------------------------------------------------
 */

#include "open_coders.hpp"

#include "xxx_common.hpp"
#include "misc/benchmarks.hpp"

using namespace std;
using namespace opc;

#define __header_written(out)   \
        do {                    \
                uint32_t        magic;  \
                uint32_t        vmajor; \
                uint32_t        vminor; \
                uint32_t        rs_num; \
                uint32_t        rs_pos; \
                uint64_t        rs_len; \
                uint64_t        rs_lenmax;      \
\
                magic = MAGIC_NUM;      \
                vmajor = VMAJOR;        \
                vminor = VMINOR;        \
\
                rs_num = 0;     \
                rs_pos = 0;     \
                rs_len = 0;     \
                rs_lenmax = UINT64_MAX; \
\
                fwrite(&magic, sizeof(uint32_t), 1, out);       \
                fwrite(&vmajor, sizeof(uint32_t), 1, out);      \
                fwrite(&vminor, sizeof(uint32_t), 1, out);      \
\
                fwrite(&rs_num, sizeof(uint32_t), 1, out);      \
                fwrite(&rs_pos, sizeof(uint32_t), 1, out);      \
                fwrite(&rs_len, sizeof(uint64_t), 1, out);      \
                fwrite(&rs_lenmax, sizeof(uint64_t), 1, out);   \
\
                fsync(fileno(out));     \
        } while (0)

#define PROG_PER_COUNT  1000000

static double   __progress_start_time;
static int      __init_progress;

#define __show_progress(str, it, c, n)  \
        ({                      \
                double  pg;     \
                double  left;   \
\
                if (!__init_progress++)         \
                        __progress_start_time = __get_time();   \
\
                if ((c) != 0 && (it) % ((n) / PROG_PER_COUNT) == 0) {   \
                        pg = (double)(c) / (n);         \
                        left = ((1.0 - pg) / pg) *      \
                                (__get_time() - __progress_start_time); \
\
                        fprintf(stderr,                         \
                                "%s: %.3lf done, %.1lfs left    \
                                                        \r",    \
                                str, pg, left);                 \
                }       \
         })

#define CHECKPOINT_INTVL        1000000       
#define __periodical_checkpoint(it, toc, cmp_pos, cmp, len, lenmax)     \
        ({      \
                if (it % CHECKPOINT_INTVL == 0) {       \
                        SET_RESUME_INFO(it, cmp_pos, len, lenmax, toc); \
                        fsync(fileno(cmp));     \
                        fsync(fileno(toc));     \
                }       \
         })

static void __usage(const char *msg, ...);

int 
main(int argc, char **argv)
{
        if (argc < 3)
                __usage(NULL);

        uint32_t *list = new uint32_t[MAXLEN];
        if (list == NULL)
                eoutput("Can't allocate memory: list");

        uint32_t *cmp_array = new uint32_t[MAXLEN];
        if (cmp_array == NULL)
                eoutput("Can't allocate memory: cmp_array");

        /* Process input options */
        int     ret;

        bool show_progress = false;
        bool try_resume = false;

        while ((ret = getopt(argc, argv, "rp")) != -1) {
                switch (ret) {
                case 'r':
                        try_resume = true;
                        break;

                case 'p':
                        show_progress = true;
                        break;

                case '?':
                        break;

                }
        }

        /* Read EncoderID */
        char            *end;

        int encID = strtol(argv[optind++], &end, 10);
        if ((*end != '\0') || (encID < 0) ||
                        (encID >= NUMENCODERS) ||(errno == ERANGE))
                __usage("EncoderID '%s' invalid", argv[1]);

        /* Open a output file and tune buffer mode */
        char            ifile[NFILENAME];
        char            ofile[NFILENAME + NEXTNAME];

        strncpy(ifile, argv[optind++], NFILENAME);
        ifile[NFILENAME - 1] = '\0';

        strncpy(ofile, ifile, NFILENAME);
        strcat(ofile, enc_ext[encID]);

        FILE *cmp = fopen(ofile, (try_resume == 0)? "w" : "r+");
        if (cmp == NULL)
                eoutput("foepn(): Can't open output files");

        strcat(ofile, TOCEXT);
        FILE *toc = fopen(ofile, (try_resume == 0)? "w" : "r+");

        if (toc == NULL)
                eoutput("foepn(): Can't open output files");

        setvbuf(cmp, NULL, _IOFBF, BUFSIZ);
        setvbuf(toc, NULL, _IOFBF, BUFSIZ);

        /* Try to resume path first */
        uint32_t it = 0;
        uint64_t len = 0;
        uint64_t rlen = 0;
        uint64_t lenmax = 0;
        uint64_t cmp_pos = 0;

        if (try_resume) {
                uint32_t        hbuf[HEADERSZ];

                uint64_t tlen = 0;

                if (fread(hbuf, sizeof(uint32_t),
                                HEADERSZ, toc) == HEADERSZ) {
                        uint64_t        pos1;
                        uint64_t        pos2;

                        __header_validate(hbuf, tlen);

                        it = GET_RESUME_NUM(hbuf);
                        cmp_pos = GET_RESUME_POS(hbuf);

                        /*
                         * A resumed posision is stored in rlen
                         * for __show_progress().
                         */
                        rlen = (len = GET_RESUME_LEN(hbuf));
                        lenmax = GET_RESUME_LENMAX(hbuf);

                        __assert(len <= lenmax);

                        pos1 = cmp_pos * sizeof(uint32_t);
                        pos2 = (HEADERSZ +
                                it * EACH_HEADER_TOC_SZ) *
                                sizeof(uint32_t);

                        if (__get_file_size(cmp) < pos1 ||
                                        __get_file_size(toc) < pos2)
                                goto NORESUME;

                        /* Locate resumed positions */
                        fseek(cmp, pos1, SEEK_SET);
                        fseek(toc, pos2, SEEK_SET);

                        goto RESUME;
                }
NORESUME:
                fseek(toc, 0, SEEK_SET);
        }

        /* First off, a header is written */
        __header_written(toc);

RESUME:
        /*
         * FIXME: I think loops with mmap() is faster than that with
         * xxxread() on most linux platforms. True?
         */
        uint64_t        fsz;

        uint32_t *addr = __open_and_mmap_file(ifile, fsz);
        lenmax = fsz >> 2;

        {
                uint32_t        prev_doc;
                uint32_t        cur_doc;
                uint32_t        cmp_size;
                uint32_t        num;

                while (len < lenmax) {
                        if (show_progress)
                                __show_progress("Encoded", it++, len, lenmax - rlen);

                        /* Read the numer of integers in a list */
                        num = __next_read32(addr, len);

                        if (len + num > lenmax)
                                goto LOOP_END;

                        /* Read the head of a list */
                        prev_doc = __next_read32(addr, len);

                        if (num > SKIP && num < MAXLEN) {
                                /*
                                 * For any list, TOC will contain:
                                 *      (number of elements, first elements, pointer to the compressed list)
                                 */
                                fwrite(&num, 1, sizeof(uint32_t), toc);
                                fwrite(&prev_doc, 1, sizeof(uint32_t), toc);
                                fwrite(&cmp_pos, 1, sizeof(uint64_t), toc);

                                for (uint32_t j = 0; j < num - 1; j++) {
                                        cur_doc = __next_read32(addr, len);

                                        if (cur_doc < prev_doc)
                                                cerr << "List ordering exception: list MUST be increasing" << endl;

                                        if (encID != E_BINARYIPL)
                                                list[j] = cur_doc - prev_doc - 1;
                                        else
                                                list[j] = cur_doc;

                                        prev_doc = cur_doc;
                                }

                                /* Do encoding */
                                (encoders[encID])(list, num - 1, cmp_array, cmp_size);

                                fwrite(cmp_array, sizeof(uint32_t), cmp_size, cmp);
                                cmp_pos += cmp_size;
                        } else {
                                /* Read skipped data */
                                for (uint32_t j = 0; j < num - 1; j++)
                                        cur_doc = __next_read32(addr, len);
                        }

                        __periodical_checkpoint(
                                        it, toc, cmp_pos, cmp, len, lenmax);
                }
        }
LOOP_END:

        /* Finalization */
        __close_file(addr, fsz);

        fclose(cmp);
        fclose(toc);

        delete[] list;
        delete[] cmp_array;

        return EXIT_SUCCESS;
}

/*--- Intra functions below ---*/

void
__usage(const char *msg, ...)
{
        if (msg != NULL) {
                va_list vargs;

                va_start(vargs, msg);
                vfprintf(stdout, msg, vargs);
                va_end(vargs);

                cout << endl;
        }

        cout << "Usage: encoders [Options] <EncoderID> <infilename>" << endl;
        cout << "Options" << endl;
        cout << "     -p: Show a progress indicator" << endl;
        cout << "     -r: Try to resume if a broken encoded file exists" << endl;

        cout << endl << "EncoderID\tEncoderName" << endl;
        cout << "---" << endl;

        cout << "\t0\tGamma" << endl;
        cout << "\t1\tDelta" << endl;
        cout << "\t2\tVariable Byte" << endl;
        cout << "\t3\tBinary Interpolative" << endl;
        cout << "\t4\tSimple 9" << endl;
        cout << "\t5\tSimple 16" << endl;
        cout << "\t6\tPForDelta" << endl; 
        cout << "\t7\tOPTPForDelta" << endl; 
        cout << "\t8\tVSEncodingBlocks" << endl;
        cout << "\t9\tVSE-R" << endl;
        cout << "\t10\tVSEncodingRest" << endl;
        cout << "\t11\tVSEncodingBlocksHybrid" << endl;
        cout << "\t12\tVSEncodingSimple v1" << endl;
        cout << "\t13\tVSEncodingSimple v2" << endl << endl;

        exit(1);
}

