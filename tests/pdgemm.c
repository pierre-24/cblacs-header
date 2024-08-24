/* Inspired by https://github.com/cjf00000/tests/blob/master/mkl/pblas3_d_example.c.
 *
 * Compute product of two NxN matrices, `C = A * B`,
 * and then check the residual `r = |B - inv(A) * C|`, where `inv(A)` is the inverse of `A`.
 *
 * In practice,
 *
 * - `A` is an orthonormal matrix (similar to a Householder transformation) defined by:
 *
 *   `A[i,j] = (N-j-1 == i ? 1:0) - 2 * (N-j-1) * i / M`, where `M = N * (N-1) * (2 * N - 1) / 6`.
 *
 *   Therefore `inv(A) = tr(A)`, where `tr(A)` is the transpose of `A`.
 *
 * - `B` is a random matrix. Here, `B[i,j] = j * N + j`.
 *
 * Thus, the residual becomes `r = |B - tr(A) * C|`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <scalapacke_blacs.h>
#include <scalapacke_pblas.h>

int I_ZERO = 0, I_ONE = 1;

// stolen from scalapack, will be replaced latter
extern int indxl2g_(int* INDXGLOB, int* NB, int* IPROC, int* ISRCPROC, int* NPROCS);
extern double pdlange_(const char* norm, const int* m, const int* n, const double* a, const int* ia, const int* ja, const int* desca, double* work);
extern int numroc_(const int* n, const int* nb, const int* iproc, const int* isrcproc, const int* nprocs);
extern void descinit_(int* desc, const int* m, const int* n, const int* mb, const int* nb, const int* irsrc, const int* icsrc, const int* ictxt, const int* lld, int* info);
extern double pdlamch_(const int* ictxt, const char* cmach);


int main(int argc, char* argv[]) {
    // constants
    int N = 256, blk_size = 32, M  = N * (N-1) * (2 * N - 1) / 6;

    // global
    int nprocs, ctx_sys, glob_nrows, glob_ncols, glob_i, glob_j;
    double norm_A, norm_B, norm_res;

    // local
    int  iam, loc_row, loc_col, loc_nrows, loc_ncols, loc_lld, info;
    int desc_A[9];
    double *A, *B, *C, *work;

    // initialize BLACS & system context
    SCALAPACKE_blacs_pinfo(&iam, &nprocs);
    SCALAPACKE_blacs_get(0, 0, &ctx_sys);

    // create the grid
    glob_nrows = (int) sqrt((double) nprocs);
    glob_ncols = nprocs / glob_nrows;

    SCALAPACKE_blacs_gridinit(&ctx_sys, "R", glob_nrows, glob_ncols);
    SCALAPACKE_blacs_gridinfo(ctx_sys, &glob_nrows, &glob_ncols, &loc_row, &loc_col);

    if(iam == 0)
        printf("%d :: grid is %dx%d, matrix is %dx%d\n", iam, glob_nrows, glob_ncols, N, N);

    if(loc_row >= 0) { // if I'm in grid
        // compute length and create arrays
        loc_nrows = numroc_(&N, &blk_size, &loc_row, &I_ZERO, &glob_nrows);
        loc_ncols = numroc_(&N, &blk_size, &loc_col, &I_ZERO, &glob_ncols);
        loc_lld =  loc_nrows;
        A = calloc(loc_nrows * loc_ncols, sizeof(double));
        B = calloc(loc_nrows * loc_ncols, sizeof(double));
        C = calloc(loc_nrows * loc_ncols, sizeof(double));

        if(A == NULL || B == NULL || C == NULL) {
            printf("%d :: cannot allocate :(\n", iam);
            exit(EXIT_FAILURE);
        } else {
            printf("%d :: local matrix is %dx%d\n", iam, loc_nrows, loc_ncols);
        }

        // fill arrays locally
        for(int loc_j=1; loc_j <= loc_ncols; loc_j++) { /* FORTRAN STARTS AT ONE !!!!!!!!! */
            // translate local j to global j
            // see https://netlib.org/scalapack/explore-html/d4/deb/indxl2g_8f_source.html
            glob_j = indxl2g_(&loc_j, &blk_size, &loc_col, &I_ZERO, &glob_ncols) - 1;
            for(int loc_i=1; loc_i <= loc_nrows; loc_i++) {
                glob_i = indxl2g_(&loc_i, &blk_size, &loc_row, &I_ZERO, &glob_nrows) - 1;

                // set A[i,j] and B[i,j]
                A[(loc_j - 1) * loc_nrows + (loc_i - 1)] = ((N - glob_j - 1) == glob_i ? 1.0 : 0.0) - (double) (2 * glob_i * (N - glob_j - 1)) / ((double) M);
                B[(loc_j - 1) * loc_nrows + (loc_i - 1)] = glob_j * N + glob_i;
            }
        }

        // create descriptor for A, B and C
        descinit_(desc_A, &N, &N, &blk_size, &blk_size, &I_ZERO, &I_ZERO, &ctx_sys, &loc_lld, &info);

        // compute norm of A and B
        work = (double*) calloc(loc_nrows, sizeof(double));
        norm_A = pdlange_( "F", &N, &N, A, &I_ONE, &I_ONE, desc_A, work);
        norm_B = pdlange_( "F", &N, &N, B, &I_ONE, &I_ONE, desc_A, work);

        // compute C = A * B
        SCALAPACKE_pdgemm("N", "N", N, N, N,
               1., A, 1, 1, desc_A,
               B, 1, 1, desc_A,
               .0, C, 1, 1, desc_A
        );

        // compute B = tr(A) * C - B
        SCALAPACKE_pdgemm("T", "N", N, N, N,
               1.0, A, 1, 1, desc_A,
               C, 1, 1, desc_A,
              -1., B, 1, 1, desc_A
        );

        // compute the norm of B & residual
        norm_res = pdlange_( "F", &N, &N, B, &I_ONE, &I_ONE, desc_A, work);
        double eps = pdlamch_(&ctx_sys, "e");
        double residual = norm_res / (2 * norm_A * norm_B * eps);

        if(iam == 0) {
            if(residual > 1.0) {
                printf("%d :: r > 1 :(", iam);
                exit(EXIT_FAILURE);
            } else
                printf("%d :: r = %f\n", iam, residual);
        }

        // free
        free(A);
        free(B);
        free(C);
        free(work);
    } else
        printf("%d :: i'm out!\n", iam);

    // finalize BLACS
    SCALAPACKE_blacs_exit(0);
    return EXIT_SUCCESS;
}
