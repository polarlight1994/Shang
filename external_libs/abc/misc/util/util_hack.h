/**CFile****************************************************************

  FileName    [util_hack.h]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [This file is used to simulate the presence of "util.h".]

  Synopsis    [External declarations.]

  Author      [Alan Mishchenko]

  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: util_hack.h,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#ifndef __UTIL_HACK_H__
#define __UTIL_HACK_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>

#include "abc_global.h"

ABC_NAMESPACE_HEADER_START

#define NIL(type)           ((type *) 0)

#define util_cpu_time       Extra_CpuTime
#define getSoftDataLimit    Extra_GetSoftDataLimit
#define MMoutOfMemory       Extra_UtilMMoutOfMemory

extern long                 Extra_CpuTime();
extern int                  Extra_GetSoftDataLimit();
extern void               (*Extra_UtilMMoutOfMemory)( long size );

ABC_NAMESPACE_HEADER_END

#endif
