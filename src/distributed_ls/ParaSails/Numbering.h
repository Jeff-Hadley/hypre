/*BHEADER**********************************************************************
 * Copyright (c) 2008,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * This file is part of HYPRE.  See file COPYRIGHT for details.
 *
 * HYPRE is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 * $Revision$
 ***********************************************************************EHEADER*/




/******************************************************************************
 *
 * Numbering.h header file.
 *
 *****************************************************************************/

#include <stdio.h>
#include "Common.h"
#include "Matrix.h"
#include "Hash.h"

#ifndef _NUMBERING_H
#define _NUMBERING_H

struct numbering
{
    HYPRE_Int   size;    /* max number of indices that can be stored */
    HYPRE_BigInt   beg_row;
    HYPRE_BigInt   end_row;
    HYPRE_Int   num_loc; /* number of local indices */
    HYPRE_Int   num_ind; /* number of indices */

    HYPRE_BigInt  *local_to_global;
    Hash *hash;
};

typedef struct numbering Numbering;

Numbering *NumberingCreate(Matrix *m, HYPRE_Int size);
Numbering *NumberingCreateCopy(Numbering *orig);
void NumberingDestroy(Numbering *numb);
void NumberingLocalToGlobal(Numbering *numb, HYPRE_Int len, HYPRE_BigInt *local, HYPRE_BigInt *global);
void NumberingGlobalToLocal(Numbering *numb, HYPRE_Int len, HYPRE_BigInt *global, HYPRE_BigInt *local);

#endif /* _NUMBERING_H */
