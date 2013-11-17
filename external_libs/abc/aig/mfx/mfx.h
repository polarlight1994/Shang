/**CFile****************************************************************

  FileName    [mfx.h]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [The good old minimization with complete don't-cares.]

  Synopsis    [External declarations.]

  Author      [Alan Mishchenko]

  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: mfx.h,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#ifndef __MFX_H__
#define __MFX_H__


////////////////////////////////////////////////////////////////////////
///                          INCLUDES                                ///
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
///                         PARAMETERS                               ///
////////////////////////////////////////////////////////////////////////



ABC_NAMESPACE_HEADER_START


////////////////////////////////////////////////////////////////////////
///                         BASIC TYPES                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Mfx_Par_t_ Mfx_Par_t;
struct Mfx_Par_t_
{
    // general parameters
    int           nWinTfoLevs;   // the maximum fanout levels
    int           nFanoutsMax;   // the maximum number of fanouts
    int           nDepthMax;     // the maximum number of logic levels
    int           nDivMax;       // the maximum number of divisors
    int           nWinSizeMax;   // the maximum size of the window
    int           nGrowthLevel;  // the maximum allowed growth in level
    int           nBTLimit;      // the maximum number of conflicts in one SAT run
    int           fResub;        // performs resubstitution
    int           fArea;         // performs optimization for area
    int           fMoreEffort;   // performs high-affort minimization
    int           fSwapEdge;     // performs edge swapping
    int           fDelay;        // performs optimization for delay
    int           fPower;        // performs power-aware optimization
    int           fVerbose;      // enable basic stats
    int           fVeryVerbose;  // enable detailed stats
};

////////////////////////////////////////////////////////////////////////
///                      MACRO DEFINITIONS                           ///
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
///                    FUNCTION DECLARATIONS                         ///
////////////////////////////////////////////////////////////////////////

/*=== mfxCore.c ==========================================================*/
extern void        Mfx_ParsDefault( Mfx_Par_t * pPars );
extern int         Mfx_Perform( Nwk_Man_t * pNtk, Mfx_Par_t * pPars, If_Lib_t * pLutLib );


ABC_NAMESPACE_HEADER_END



#endif

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////
