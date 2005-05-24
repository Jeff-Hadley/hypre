/*
   Example 5

   Interface:    Linear-Algebraic (IJ), Babel-based version

   Compile with: make ex5

   Sample run:   mpirun -np 4 ex5

   Description:  This example solves the 2-D
                 Laplacian problem with zero boundary conditions
                 on an nxn grid.  The number of unknowns is N=n^2.
                 The standard 5-point stencil is used, and we solve
                 for the interior nodes only.

                 This example solves the same problem as Example 3.
                 Available solvers are AMG, PCG, and PCG with AMG or
                 Parasails preconditioners.
*/

#include <math.h>
#include <assert.h>
#include "utilities.h"
#include "krylov.h"
#include "HYPRE.h"
#include "HYPRE_parcsr_ls.h"
#include "bHYPRE.h"
#include "bHYPRE_Vector.h"
#include "bHYPRE_IJParCSRMatrix.h"
#include "bHYPRE_IJParCSRVector.h"
#include "bHYPRE_ParCSRDiagScale.h"
#include "bHYPRE_BoomerAMG.h"

int main (int argc, char *argv[])
{
   int i;
   int myid, num_procs;
   int N, n;

   int ilower, iupper;
   int local_size, extra;

   int solver_id;
   int print_solution;

   double h, h2;

   int ierr = 0;
   /* If this gets set to anything else, it's an error.
    Most functions return error flags, 0 unless there's an error.
    For clarity, they aren't checked much in this file. */

   int zero = 0;
   int one  = 1;
   int five = 5;

   bHYPRE_IJParCSRMatrix parcsr_A;
   bHYPRE_Operator           op_A;
   sidl_BaseInterface * base_object;
   bHYPRE_IJParCSRVector par_b;
   bHYPRE_IJParCSRVector par_x;
   bHYPRE_Vector vec_b;
   bHYPRE_Vector vec_x;

   bHYPRE_Solver solver, precond;
   bHYPRE_BoomerAMG amg_solver;
   bHYPRE_PCG pcg_solver;
   bHYPRE_IdentitySolver identity;

   /* Initialize MPI */
   MPI_Init(&argc, &argv);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);

   /* Default problem parameters */
   n = 33;
   solver_id = 0;
   print_solution  = 0;

   /* Parse command line */
   {
      int arg_index = 0;
      int print_usage = 0;

      while (arg_index < argc)
      {
         if ( strcmp(argv[arg_index], "-n") == 0 )
         {
            arg_index++;
            n = atoi(argv[arg_index++]);
         }
         else if ( strcmp(argv[arg_index], "-solver") == 0 )
         {
            arg_index++;
            solver_id = atoi(argv[arg_index++]);
         }
         else if ( strcmp(argv[arg_index], "-print_solution") == 0 )
         {
            arg_index++;
            print_solution = 1;
         }
         else if ( strcmp(argv[arg_index], "-help") == 0 )
         {
            print_usage = 1;
            break;
         }
         else
         {
            arg_index++;
         }
      }

      if ((print_usage) && (myid == 0))
      {
         printf("\n");
         printf("Usage: %s [<options>]\n", argv[0]);
         printf("\n");
         printf("  -n <n>              : problem size in each direction (default: 33)\n");
         printf("  -solver <ID>        : solver ID\n");
         printf("                        0  - AMG (default) \n");
         printf("                        1  - AMG-PCG\n");
         printf("                        8  - ParaSails-PCG\n");
         printf("                        50 - PCG\n");
         printf("  -print_solution     : print the solution vector\n");
         printf("\n");
      }

      if (print_usage)
      {
         MPI_Finalize();
         return (0);
      }
   }

   /* Preliminaries: want at least one processor per row */
   if (n*n < num_procs) n = sqrt(num_procs) + 1;
   N = n*n; /* global number of rows */
   h = 1.0/(n+1); /* mesh size*/
   h2 = h*h;

   /* Each processor knows only of its own rows - the range is denoted by ilower
      and upper.  Here we partition the rows. We account for the fact that
      N may not divide evenly by the number of processors. */
   local_size = N/num_procs;
   extra = N - local_size*num_procs;

   ilower = local_size*myid;
   ilower += hypre_min(myid, extra);

   iupper = local_size*(myid+1);
   iupper += hypre_min(myid+1, extra);
   iupper = iupper - 1;

   /* How many rows do I have? */
   local_size = iupper - ilower + 1;

   /* Create the matrix.
      Note that this is a square matrix, so we indicate the row partition
      size twice (since number of rows = number of cols) */
   parcsr_A = bHYPRE_IJParCSRMatrix__create();
   bHYPRE_IJParCSRMatrix_SetCommunicator( parcsr_A, (void * )MPI_COMM_WORLD );
   bHYPRE_IJParCSRMatrix_SetLocalRange( parcsr_A, ilower, iupper, ilower, iupper );

   op_A = bHYPRE_Operator__cast( parcsr_A ); /* needed later as a function argument */

   /* Choose a parallel csr format storage (see the User's Manual) */
   /* Note: Here the HYPRE interface requires a SetObjectType call.
      I am using the bHYPRE interface in a way which does not because
      the object type is already specified through the class name. */

   /* Initialize before setting coefficients */
   bHYPRE_IJParCSRMatrix_Initialize( parcsr_A );

   /* Now go through my local rows and set the matrix entries.
      Each row has at most 5 entries. For example, if n=3:

      A = [M -I 0; -I M -I; 0 -I M]
      M = [4 -1 0; -1 4 -1; 0 -1 4]

      Note that here we are setting one row at a time, though
      one could set all the rows together (see the User's Manual).
   */
   {
      int nnz;
      double values[5];
      int cols[5];
      struct sidl_int__array* s_nnz;
      struct sidl_int__array* s_i;
      struct sidl_int__array* s_cols;
      struct sidl_double__array* s_values;
      for (i = ilower; i <= iupper; i++)
      {
         nnz = 0;

         /* The left identity block:position i-n */
         if ((i-n)>=0)
         {
	    cols[nnz] = i-n;
	    values[nnz] = -1.0;
	    nnz++;
         }

         /* The left -1: position i-1 */
         if (i%n)
         {
            cols[nnz] = i-1;
            values[nnz] = -1.0;
            nnz++;
         }

         /* Set the diagonal: position i */
         cols[nnz] = i;
         values[nnz] = 4.0;
         nnz++;

         /* The right -1: position i+1 */
         if ((i+1)%n)
         {
            cols[nnz] = i+1;
            values[nnz] = -1.0;
            nnz++;
         }

         /* The right identity block:position i+n */
         if ((i+n)< N)
         {
            cols[nnz] = i+n;
            values[nnz] = -1.0;
            nnz++;
         }

         /* Set up sidl arrays for the Babel interface.
            The "borrow" command simply wraps the data with meta information -
            dimension, index range, and stride of the array. */
         s_nnz =  sidl_int__array_borrow( &nnz, 1, &zero, &one, &one );
         s_i =    sidl_int__array_borrow( &i,   1, &zero, &one, &one );
         s_cols = sidl_int__array_borrow( cols, 1, &zero, &five, &one );
         s_values = sidl_double__array_borrow( values, 1, &zero, &five, &one );

         /* Set the values for row i */
         bHYPRE_IJParCSRMatrix_SetValues( parcsr_A, 1, s_nnz, s_i, s_cols, s_values );

         /* Clean up the sidl arrays: */
         sidl_int__array_deleteRef( s_nnz );
         sidl_int__array_deleteRef( s_i );
         sidl_int__array_deleteRef( s_cols );
         sidl_double__array_deleteRef( s_values );
      }
   }

   /* Assemble after setting the coefficients */
   bHYPRE_IJParCSRMatrix_Assemble( parcsr_A );

   /* Create the rhs and solution */
   par_b = bHYPRE_IJParCSRVector__create();
   bHYPRE_IJParCSRVector_SetCommunicator( par_b, (void *)MPI_COMM_WORLD );
   bHYPRE_IJParCSRVector_SetLocalRange( par_b, ilower, iupper );
   vec_b = bHYPRE_Vector__cast( par_b ); /* needed later for function arguments */

   bHYPRE_IJParCSRVector_Initialize( par_b );

   par_x = bHYPRE_IJParCSRVector__create();
   bHYPRE_IJParCSRVector_SetCommunicator( par_x, (void *)MPI_COMM_WORLD );
   bHYPRE_IJParCSRVector_SetLocalRange( par_x, ilower, iupper );
   vec_x = bHYPRE_Vector__cast( par_x ); /* needed later for function arguments */

   bHYPRE_IJParCSRVector_Initialize( par_x );

   /* Set the rhs values to h^2 and the solution to zero */
   {
      double *rhs_values, *x_values;
      int    *rows;
      struct sidl_double__array* s_rhs_values;
      struct sidl_double__array* s_x_values;
      struct sidl_int__array* s_rows;

      rhs_values = calloc(local_size, sizeof(double));
      x_values = calloc(local_size, sizeof(double));
      rows = calloc(local_size, sizeof(int));

      for (i=0; i<local_size; i++)
      {
         rhs_values[i] = h2;
         x_values[i] = 0.0;
         rows[i] = ilower + i;
      }

      s_rhs_values = sidl_double__array_borrow( rhs_values, 1, &zero, &local_size, &one );
      s_x_values   = sidl_double__array_borrow(   x_values, 1, &zero, &local_size, &one );
      s_rows   = sidl_int__array_borrow(   rows, 1, &zero, &local_size, &one );

      bHYPRE_IJParCSRVector_SetValues( par_b, local_size, s_rows, s_rhs_values );
      bHYPRE_IJParCSRVector_SetValues( par_x, local_size, s_rows, s_x_values );

      sidl_double__array_deleteRef( s_rhs_values );
      sidl_double__array_deleteRef( s_x_values );
      sidl_int__array_deleteRef( s_rows );
      free(x_values);
      free(rhs_values);
      free(rows);
   }

   bHYPRE_IJParCSRVector_Assemble( par_b );
   bHYPRE_IJParCSRVector_Assemble( par_x );

   /* Choose a solver and solve the system */

   /* AMG */
   if (solver_id == 0)
   {
      int num_iterations;
      double final_res_norm;

      /* Create solver */
      amg_solver = bHYPRE_BoomerAMG__create();
      bHYPRE_BoomerAMG_SetCommunicator( amg_solver, (void *)MPI_COMM_WORLD );
      bHYPRE_BoomerAMG_SetOperator( amg_solver, op_A );

      /* Set some parameters (See Reference Manual for more parameters) */
      bHYPRE_BoomerAMG_SetPrintLevel( amg_solver, 3 );  /* print solve info + parameters */
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "CoarsenType", 6); /* Falgout coarsening */
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "RelaxType", 3);   /* G-S/Jacobi hybrid relaxation */
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "NumSweeps", 1);   /* Sweeeps on each level */
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "MaxLevels", 20);  /* maximum number of levels */
      bHYPRE_BoomerAMG_SetTolerance( amg_solver, 1e-7);      /* conv. tolerance */

      /* Now setup and solve! */
      bHYPRE_BoomerAMG_Setup( amg_solver, vec_b, vec_x );
      bHYPRE_BoomerAMG_Apply( amg_solver, vec_b, &vec_x );

      /* Run info - needed logging turned on */
      bHYPRE_BoomerAMG_GetNumIterations( amg_solver, &num_iterations );
      bHYPRE_BoomerAMG_GetRelResidualNorm( amg_solver, &final_res_norm );

      if (myid == 0)
      {
         printf("\n");
         printf("Iterations = %d\n", num_iterations);
         printf("Final Relative Residual Norm = %e\n", final_res_norm);
         printf("\n");
      }

      /* Destroy solver */
      bHYPRE_BoomerAMG_deleteRef( amg_solver );
   }

   /* PCG */
   else if (solver_id == 50)
   {
      int num_iterations;
      double final_res_norm;

      /* Create solver */
      pcg_solver = bHYPRE_PCG__create();
      bHYPRE_PCG_SetCommunicator( pcg_solver, (void *)MPI_COMM_WORLD );
      bHYPRE_PCG_SetOperator( pcg_solver, op_A );

      /* Set some parameters (See Reference Manual for more parameters) */
      bHYPRE_PCG_SetIntParameter( pcg_solver, "MaxIter", 1000 ); /* max iterations */
      bHYPRE_PCG_SetTolerance( pcg_solver, 1e-7 ); /* conv. tolerance */
      bHYPRE_PCG_SetIntParameter( pcg_solver, "TwoNorm", 1 ); /* use the two norm as the stopping criteria */
      bHYPRE_PCG_SetPrintLevel( pcg_solver, 2 ); /* prints out the iteration info */
      bHYPRE_PCG_SetLogging( pcg_solver, 1 ); /* needed to get run info later */

      identity = bHYPRE_IdentitySolver__create();
      precond = bHYPRE_Solver__cast( identity );
      bHYPRE_PCG_SetPreconditioner( pcg_solver, precond );

      /* Now setup and solve! */
      bHYPRE_PCG_Setup( pcg_solver, vec_b, vec_x );
      bHYPRE_PCG_Apply( pcg_solver, vec_b, &vec_x );

      /* Run info - needed logging turned on */
      bHYPRE_PCG_GetNumIterations( pcg_solver, &num_iterations );
      bHYPRE_PCG_GetRelResidualNorm( pcg_solver, &final_res_norm );
      if (myid == 0)
      {
         printf("\n");
         printf("Iterations = %d\n", num_iterations);
         printf("Final Relative Residual Norm = %e\n", final_res_norm);
         printf("\n");
      }

      /* Destroy solvers */
      bHYPRE_PCG_deleteRef( pcg_solver );
      bHYPRE_Solver_deleteRef( precond );
   }
   /* PCG with AMG preconditioner */
   else if (solver_id == 1)
   {
      int num_iterations;
      double final_res_norm;

      /* Create solver */
      pcg_solver = bHYPRE_PCG__create();
      bHYPRE_PCG_SetCommunicator( pcg_solver, (void *)MPI_COMM_WORLD );
      bHYPRE_PCG_SetOperator( pcg_solver, op_A );

      /* Set some parameters (See Reference Manual for more parameters) */
      bHYPRE_PCG_SetIntParameter( pcg_solver, "MaxIter", 1000 ); /* max iterations */
      bHYPRE_PCG_SetTolerance( pcg_solver, 1e-7 ); /* conv. tolerance */
      bHYPRE_PCG_SetIntParameter( pcg_solver, "TwoNorm", 1 ); /* use the two norm as the stopping criteria */
      bHYPRE_PCG_SetPrintLevel( pcg_solver, 2 ); /* prints out the iteration info */
      bHYPRE_PCG_SetLogging( pcg_solver, 1 ); /* needed to get run info later */

      /* Now set up the AMG preconditioner and specify any parameters */
      amg_solver = bHYPRE_BoomerAMG__create();
      bHYPRE_BoomerAMG_SetCommunicator( amg_solver, (void *)MPI_COMM_WORLD );
      bHYPRE_BoomerAMG_SetOperator( amg_solver, op_A );
      bHYPRE_BoomerAMG_SetPrintLevel( amg_solver, 1 ); /* print amg solution info*/
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "CoarsenType", 6); /* Falgout coarsening */
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "RelaxType", 3);   /* G-S/Jacobi hybrid relaxation */
      bHYPRE_BoomerAMG_SetIntParameter( amg_solver, "NumSweeps", 1);   /* Sweeeps on each level */
      bHYPRE_BoomerAMG_SetTolerance( amg_solver, 1e-3);      /* conv. tolerance */

      /* Set the PCG preconditioner */
      precond = bHYPRE_Solver__cast( amg_solver );
      bHYPRE_PCG_SetPreconditioner( pcg_solver, precond );

      /* Now setup and solve! */
      bHYPRE_PCG_Setup( pcg_solver, vec_b, vec_x );
      bHYPRE_PCG_Apply( pcg_solver, vec_b, &vec_x );

      /* Run info - needed logging turned on */
      bHYPRE_PCG_GetNumIterations( pcg_solver, &num_iterations );
      bHYPRE_PCG_GetRelResidualNorm( pcg_solver, &final_res_norm );
      if (myid == 0)
      {
         printf("\n");
         printf("Iterations = %d\n", num_iterations);
         printf("Final Relative Residual Norm = %e\n", final_res_norm);
         printf("\n");
      }

      /* Destroy solver and preconditioner */
      bHYPRE_PCG_deleteRef( pcg_solver );
      bHYPRE_Solver_deleteRef( precond );
   }
#if 0 /* waiting for Parasails Babel interface to be written
   /* PCG with Parasails Preconditioner */
   else if (solver_id == 8)
   {
      int    num_iterations;
      double final_res_norm;

      int      sai_max_levels = 1;
      double   sai_threshold = 0.1;
      double   sai_filter = 0.05;
      int      sai_sym = 1;

      /* Create solver */
      pcg_solver = bHYPRE_PCG__create();
      bHYPRE_PCG_SetCommunicator( pcg_solver, (void *)MPI_COMM_WORLD );
      bHYPRE_PCG_SetOperator( pcg_solver, op_A );

      /* Set some parameters (See Reference Manual for more parameters) */
      bHYPRE_PCG_SetIntParameter( pcg_solver, "MaxIter", 1000 ); /* max iterations */
      bHYPRE_PCG_SetTolerance( pcg_solver, 1e-7 ); /* conv. tolerance */
      bHYPRE_PCG_SetIntParameter( pcg_solver, "TwoNorm", 1 ); /* use the two norm as the stopping criteria */
      bHYPRE_PCG_SetPrintLevel( pcg_solver, 2 ); /* prints out the iteration info */
      bHYPRE_PCG_SetLogging( pcg_solver, 1 ); /* needed to get run info later */

      /* Now set up the ParaSails preconditioner and specify any parameters */
      HYPRE_ParaSailsCreate(MPI_COMM_WORLD, &precond);

      /* Set some parameters (See Reference Manual for more parameters) */
      HYPRE_ParaSailsSetParams(precond, sai_threshold, sai_max_levels);
      HYPRE_ParaSailsSetFilter(precond, sai_filter);
      HYPRE_ParaSailsSetSym(precond, sai_sym);
      HYPRE_ParaSailsSetLogging(precond, 3);

      /* Set the PCG preconditioner */
      HYPRE_PCGSetPrecond(solver, (HYPRE_PtrToSolverFcn) HYPRE_ParaSailsSolve,
                          (HYPRE_PtrToSolverFcn) HYPRE_ParaSailsSetup, precond);

      /* Now setup and solve! */
      HYPRE_ParCSRPCGSetup(solver, parcsr_A, par_b, par_x);
      HYPRE_ParCSRPCGSolve(solver, parcsr_A, par_b, par_x);


      /* Run info - needed logging turned on */
      HYPRE_PCGGetNumIterations(solver, &num_iterations);
      HYPRE_PCGGetFinalRelativeResidualNorm(solver, &final_res_norm);
      if (myid == 0)
      {
         printf("\n");
         printf("Iterations = %d\n", num_iterations);
         printf("Final Relative Residual Norm = %e\n", final_res_norm);
         printf("\n");
      }

      /* Destory solver and preconditioner */
      HYPRE_ParCSRPCGDestroy(solver);
      HYPRE_ParaSailsDestroy(precond);
   }
#endif
   else
   {
      if (myid ==0) printf("Invalid solver id specified.\n");
   }

   /* Print the solution */
   if (print_solution)
      bHYPRE_IJParCSRVector_Print( par_x, "ij.out.x" );

   /* Clean up */
   bHYPRE_IJParCSRMatrix_deleteRef( parcsr_A );
   bHYPRE_IJParCSRVector_deleteRef( par_b );
   bHYPRE_IJParCSRVector_deleteRef( par_x );

   assert( ierr == 0 );

   /* Finalize MPI*/
   MPI_Finalize();

   return(0);
}