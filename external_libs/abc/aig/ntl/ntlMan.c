/**CFile****************************************************************

  FileName    [ntlMan.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Netlist representation.]

  Synopsis    [Netlist manager.]

  Author      [Alan Mishchenko]

  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: ntlMan.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "ntl.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    [Allocates the netlist manager.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Man_t * Ntl_ManAlloc()
{
    Ntl_Man_t * p;
    // start the manager
    p = ABC_ALLOC( Ntl_Man_t, 1 );
    memset( p, 0, sizeof(Ntl_Man_t) );
    p->vModels = Vec_PtrAlloc( 1000 );
    p->vCis = Vec_PtrAlloc( 1000 );
    p->vCos = Vec_PtrAlloc( 1000 );
    p->vVisNodes = Vec_PtrAlloc( 1000 );
    p->vBox1Cios = Vec_IntAlloc( 1000 );
    p->vRegClasses = Vec_IntAlloc( 1000 );
    p->vRstClasses = Vec_IntAlloc( 1000 );
    // start the manager
    p->pMemObjs = Aig_MmFlexStart();
    p->pMemSops = Aig_MmFlexStart();
    // allocate model table
    p->nModTableSize = Aig_PrimeCudd( 100 );
    p->pModTable = ABC_ALLOC( Ntl_Mod_t *, p->nModTableSize );
    memset( p->pModTable, 0, sizeof(Ntl_Mod_t *) * p->nModTableSize );
    return p;
}

/**Function*************************************************************

  Synopsis    [Cleanups extended representation.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManCleanup( Ntl_Man_t * p )
{
    if ( p->pAig )
    {
        Aig_ManStop( p->pAig );
        p->pAig = NULL;
    }
    if ( p->pManTime )
    {
        Tim_ManStop( p->pManTime );
        p->pManTime = NULL;
    }
}

/**Function*************************************************************

  Synopsis    [Duplicates the design without the nodes of the root model.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Man_t * Ntl_ManStartFrom( Ntl_Man_t * pOld )
{
    Ntl_Man_t * pNew;
    Ntl_Mod_t * pModel;
    Ntl_Obj_t * pBox;
    Ntl_Net_t * pNet;
    int i, k;
    pNew = Ntl_ManAlloc();
    pNew->pName = Ntl_ManStoreFileName( pNew, pOld->pName );
    pNew->pSpec = Ntl_ManStoreName( pNew, pOld->pName );
    Vec_PtrForEachEntry( Ntl_Mod_t *, pOld->vModels, pModel, i )
    {
        if ( i == 0 )
        {
            Ntl_ManMarkCiCoNets( pOld );
            pModel->pCopy = Ntl_ModelStartFrom( pNew, pModel );
            Ntl_ManUnmarkCiCoNets( pOld );
        }
        else
            pModel->pCopy = Ntl_ModelDup( pNew, pModel );
    }
    Vec_PtrForEachEntry( Ntl_Mod_t *, pOld->vModels, pModel, i )
        Ntl_ModelForEachBox( pModel, pBox, k )
        {
            ((Ntl_Obj_t *)pBox->pCopy)->pImplem = (Ntl_Mod_t *)pBox->pImplem->pCopy;
            ((Ntl_Obj_t *)pBox->pCopy)->iTemp = pBox->iTemp;
//            ((Ntl_Obj_t *)pBox->pCopy)->Reset = pBox->Reset;
        }
    Ntl_ManForEachCiNet( pOld, pNet, i )
        Vec_PtrPush( pNew->vCis, pNet->pCopy );
    Ntl_ManForEachCoNet( pOld, pNet, i )
        Vec_PtrPush( pNew->vCos, pNet->pCopy );
    if ( pOld->pManTime )
        pNew->pManTime = Tim_ManDup( pOld->pManTime, 0 );
    if ( pOld->pNal )
        pOld->pNalD( pOld, pNew );
    return pNew;
}

/**Function*************************************************************

  Synopsis    [Duplicates the design.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Man_t * Ntl_ManDup( Ntl_Man_t * pOld )
{
    Ntl_Man_t * pNew;
    Ntl_Mod_t * pModel;
    Ntl_Obj_t * pBox;
    Ntl_Net_t * pNet;
    int i, k;
    pNew = Ntl_ManAlloc();
    pNew->pName = Ntl_ManStoreFileName( pNew, pOld->pName );
    pNew->pSpec = Ntl_ManStoreName( pNew, pOld->pName );
    Vec_PtrForEachEntry( Ntl_Mod_t *, pOld->vModels, pModel, i )
        pModel->pCopy = Ntl_ModelDup( pNew, pModel );
    Vec_PtrForEachEntry( Ntl_Mod_t *, pOld->vModels, pModel, i )
        Ntl_ModelForEachBox( pModel, pBox, k )
            ((Ntl_Obj_t *)pBox->pCopy)->pImplem = (Ntl_Mod_t *)pBox->pImplem->pCopy;
    Ntl_ManForEachCiNet( pOld, pNet, i )
        Vec_PtrPush( pNew->vCis, pNet->pCopy );
    Ntl_ManForEachCoNet( pOld, pNet, i )
        Vec_PtrPush( pNew->vCos, pNet->pCopy );
    if ( pOld->pManTime )
        pNew->pManTime = Tim_ManDup( pOld->pManTime, 0 );
    if ( !Ntl_ManCheck( pNew ) )
        printf( "Ntl_ManDup: The check has failed for design %s.\n", pNew->pName );
    return pNew;
}

/**Function*************************************************************

  Synopsis    [Duplicates the design.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Man_t * Ntl_ManDupCollapseLuts( Ntl_Man_t * pOld )
{
    Ntl_Man_t * pNew;
    Ntl_Mod_t * pModel;
    Ntl_Obj_t * pBox;
//    Ntl_Net_t * pNet;
    int i, k;
    pNew = Ntl_ManAlloc();
    pNew->pName = Ntl_ManStoreFileName( pNew, pOld->pName );
    pNew->pSpec = Ntl_ManStoreName( pNew, pOld->pName );
    Vec_PtrForEachEntry( Ntl_Mod_t *, pOld->vModels, pModel, i )
        pModel->pCopy = Ntl_ModelDupCollapseLuts( pNew, pModel );
    Vec_PtrForEachEntry( Ntl_Mod_t *, pOld->vModels, pModel, i )
        Ntl_ModelForEachBox( pModel, pBox, k )
            if ( pBox->pCopy )
                ((Ntl_Obj_t *)pBox->pCopy)->pImplem = (Ntl_Mod_t *)pBox->pImplem->pCopy;
//    Ntl_ManForEachCiNet( pOld, pNet, i )
//        Vec_PtrPush( pNew->vCis, pNet->pCopy );
//    Ntl_ManForEachCoNet( pOld, pNet, i )
//        Vec_PtrPush( pNew->vCos, pNet->pCopy );
//    if ( pOld->pManTime )
//        pNew->pManTime = Tim_ManDup( pOld->pManTime, 0 );
    if ( !Ntl_ManCheck( pNew ) )
        printf( "Ntl_ManDup: The check has failed for design %s.\n", pNew->pName );
    return pNew;
}

/**Function*************************************************************

  Synopsis    [Deallocates the netlist manager.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManFree( Ntl_Man_t * p )
{
    if ( p->vModels )
    {
        Ntl_Mod_t * pModel;
        int i;
        Ntl_ManForEachModel( p, pModel, i )
            Ntl_ModelFree( pModel );
        Vec_PtrFree( p->vModels );
    }
    if ( p->vCis )       Vec_PtrFree( p->vCis );
    if ( p->vCos )       Vec_PtrFree( p->vCos );
    if ( p->vVisNodes )  Vec_PtrFree( p->vVisNodes );
    if ( p->vRegClasses) Vec_IntFree( p->vRegClasses );
    if ( p->vRstClasses) Vec_IntFree( p->vRstClasses );
    if ( p->vBox1Cios )  Vec_IntFree( p->vBox1Cios );
    if ( p->pMemObjs )   Aig_MmFlexStop( p->pMemObjs, 0 );
    if ( p->pMemSops )   Aig_MmFlexStop( p->pMemSops, 0 );
    if ( p->pAig )       Aig_ManStop( p->pAig );
    if ( p->pManTime )   Tim_ManStop( p->pManTime );
    if ( p->pNal )       p->pNalF( p->pNal );
    ABC_FREE( p->pModTable );
    ABC_FREE( p );
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManPrintStats( Ntl_Man_t * p )
{
    Ntl_Mod_t * pRoot;
    pRoot = Ntl_ManRootModel( p );
    printf( "%-15s : ",        p->pName );
    printf( "pi = %5d  ",      Ntl_ModelPiNum(pRoot) );
    printf( "po = %5d  ",      Ntl_ModelPoNum(pRoot) );
    printf( "lat = %5d  ",     Ntl_ModelLatchNum(pRoot) );
    printf( "node = %5d  ",    Ntl_ModelNodeNum(pRoot) );
    printf( "\n    " );
    printf( "inv/buf = %5d  ", Ntl_ModelLut1Num(pRoot) );
    printf( "box = %4d  ",     Ntl_ModelBoxNum(pRoot) );
    printf( "mod = %3d  ",     Vec_PtrSize(p->vModels) );
    printf( "net = %d",        Ntl_ModelCountNets(pRoot) );
    printf( "\n" );
    fflush( stdout );
    assert( Ntl_ModelLut1Num(pRoot) == Ntl_ModelCountLut1(pRoot) );
    Ntl_ManPrintTypes( p );
    fflush( stdout );
}


/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Nwk_ManPrintStatsShort( Ntl_Man_t * p, Aig_Man_t * pAig, Nwk_Man_t * pNtk )
{
    Ntl_Mod_t * pRoot;
	Ntl_Obj_t * pObj;
	int i, Counter = 0;
    pRoot = Ntl_ManRootModel( p );
	Ntl_ModelForEachBox( pRoot, pObj, i )
		if ( strcmp(pObj->pImplem->pName, "dff") == 0 )
			Counter++;
    if ( Counter == 0 )
    {
	    Ntl_ModelForEachBox( pRoot, pObj, i )
            Counter += (pObj->pImplem->attrWhite && !pObj->pImplem->attrComb);
    }
    printf( "%-15s : ",  p->pName );
    printf( "pi =%5d  ", Ntl_ModelPiNum(pRoot) );
    printf( "po =%5d  ", Ntl_ModelPoNum(pRoot) );
    printf( "ff =%5d  ", Counter );
    printf( "box =%6d  ", Ntl_ModelBoxNum(pRoot) );
    if ( pAig != NULL )
    {
        Counter = Aig_ManChoiceNum( pAig );
        if ( Counter )
            printf( "cho =%7d  ", Counter );
        else
            printf( "aig =%7d  ", Aig_ManNodeNum(pAig) );
    }
	if ( pNtk == NULL )
		printf( "No mapping.\n" );
	else
	{
		printf( "lut =%5d  ",  Nwk_ManNodeNum(pNtk) );
		printf( "lev =%3d  ",  Nwk_ManLevel(pNtk) );
//		printf( "del =%5.2f ", Nwk_ManDelayTraceLut(pNtk) );
		printf( "\n" );
	}
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ManStatsRegs( Ntl_Man_t * p )
{
    Ntl_Mod_t * pRoot;
	Ntl_Obj_t * pObj;
	int i, Counter = 0;
    pRoot = Ntl_ManRootModel( p );
	Ntl_ModelForEachBox( pRoot, pObj, i )
		if ( strcmp(pObj->pImplem->pName, "m_dff") == 0 )
			Counter++;
    return Counter;
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ManStatsLuts( Ntl_Man_t * p )
{
    return Ntl_ModelLut1Num( Ntl_ManRootModel(p) ) + Ntl_ModelNodeNum( Ntl_ManRootModel(p) );
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Nwk_ManStatsLuts( Nwk_Man_t * pNtk )
{
    return pNtk ? Nwk_ManNodeNum(pNtk) : -1;
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Nwk_ManStatsLevs( Nwk_Man_t * pNtk )
{
    return pNtk ? Nwk_ManLevel(pNtk) : -1;
}


/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Nwk_ManPrintStatsUpdate( Ntl_Man_t * p, Aig_Man_t * pAig, Nwk_Man_t * pNtk,
     int nRegInit, int nLutInit, int nLevInit, int Time )
{
    printf( "FF =%7d (%5.1f%%)  ",  Ntl_ManStatsRegs(p),    nRegInit ? (100.0*(nRegInit-Ntl_ManStatsRegs(p))/nRegInit) : 0.0 );
    if ( pNtk == NULL )
        printf( "Mapping is not available.                    " );
    else
    {
        printf( "Lut =%7d (%5.1f%%)  ", Ntl_ManStatsLuts(p), nLutInit ? (100.0*(nLutInit-Ntl_ManStatsLuts(p))/nLutInit) : 0.0 );
        printf( "Lev =%4d (%5.1f%%)    ", Nwk_ManStatsLevs(pNtk), nLevInit ? (100.0*(nLevInit-Nwk_ManStatsLevs(pNtk))/nLevInit) : 0.0 );
    }
    ABC_PRT( "Time", clock() - Time );
}


/**Function*************************************************************

  Synopsis    [Deallocates the netlist manager.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Tim_Man_t * Ntl_ManReadTimeMan( Ntl_Man_t * p )
{
    return p->pManTime;
}

/**Function*************************************************************

  Synopsis    [Saves the model type.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManSaveBoxType( Ntl_Obj_t * pObj )
{
    Ntl_Mod_t * pModel = pObj->pImplem;
    int Number = 0;
    assert( Ntl_ObjIsBox(pObj) );
    Number |= (pModel->attrWhite   << 0);
    Number |= (pModel->attrBox     << 1);
    Number |= (pModel->attrComb    << 2);
    Number |= (pModel->attrKeep    << 3);
    Number |= (pModel->attrNoMerge << 4);
    pModel->pMan->BoxTypes[Number]++;
}

/**Function*************************************************************

  Synopsis    [Saves the model type.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManPrintTypes( Ntl_Man_t * p )
{
    Vec_Ptr_t * vFlops;
    Ntl_Net_t * pNet;
    Ntl_Mod_t * pModel;
    Ntl_Obj_t * pObj;
    int i;
    pModel = Ntl_ManRootModel( p );
    if ( Ntl_ModelBoxNum(pModel) == 0 )
        return;
    printf( "BOX STATISTICS:\n" );
    Ntl_ModelForEachBox( pModel, pObj, i )
        Ntl_ManSaveBoxType( pObj );
    for ( i = 0; i < 32; i++ )
    {
        if ( !p->BoxTypes[i] )
            continue;
        printf( "Type %2d  Num = %7d :", i, p->BoxTypes[i] );
        printf( " %s", ((i &  1) > 0)? "white   ": "black   " );
        printf( " %s", ((i &  2) > 0)? "box     ": "logic   " );
        printf( " %s", ((i &  4) > 0)? "comb    ": "seq     " );
        printf( " %s", ((i &  8) > 0)? "keep    ": "sweep   " );
        printf( " %s", ((i & 16) > 0)? "no_merge": "merge   " );
        printf( "\n" );
    }
    printf( "MODEL STATISTICS:\n" );
    Ntl_ManForEachModel( p, pModel, i )
        if ( i ) printf( "Model %2d :  Name = %10s  Used = %6d.\n", i, pModel->pName, pModel->nUsed );
    for ( i = 0; i < 32; i++ )
        p->BoxTypes[i] = 0;
    pModel = Ntl_ManRootModel( p );
    if ( pModel->vClockFlops )
    {
        printf( "CLOCK STATISTICS:\n" );
        Vec_VecForEachLevel( pModel->vClockFlops, vFlops, i )
        {
            pNet = (Ntl_Net_t *)Vec_PtrEntry( pModel->vClocks, i );
            printf( "Clock %2d :  Name = %30s   Flops = %6d.\n", i+1, pNet->pName, Vec_PtrSize(vFlops) );
        }
    }
    printf( "\n" );
}

/**Function*************************************************************

  Synopsis    [Procedure used for sorting the nodes in decreasing order of levels.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ManCompareClockClasses( Vec_Ptr_t ** pp1, Vec_Ptr_t ** pp2 )
{
    int Diff = Vec_PtrSize(*pp1) - Vec_PtrSize(*pp2);
    if ( Diff > 0 )
        return -1;
    if ( Diff < 0 )
        return 1;
    return 0;
}

/**Function*************************************************************

  Synopsis    [Saves the model type.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManPrintClocks( Ntl_Man_t * p )
{
    Vec_Ptr_t * vFlops;
    Ntl_Net_t * pNet;
    Ntl_Mod_t * pModel;
    int i;
    pModel = Ntl_ManRootModel( p );
    if ( Ntl_ModelBoxNum(pModel) == 0 )
        return;
    if ( pModel->vClockFlops )
    {
        printf( "CLOCK STATISTICS:\n" );
        Vec_VecForEachLevel( pModel->vClockFlops, vFlops, i )
        {
            pNet = (Ntl_Net_t *)Vec_PtrEntry( pModel->vClocks, i );
            printf( "Clock %2d :  Name = %30s   Flops = %6d.\n", i+1, pNet->pName, Vec_PtrSize(vFlops) );
            if ( i == 10 )
            {
                printf( "Skipping... (the total is %d)\n", Vec_VecSize(pModel->vClockFlops) );
                break;
            }
        }
    }
//    printf( "\n" );
}

/**Function*************************************************************

  Synopsis    [Saves the model type.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ManPrintResets( Ntl_Man_t * p )
{
    Vec_Ptr_t * vFlops;
    Ntl_Net_t * pNet;
    Ntl_Mod_t * pModel;
    int i;
    pModel = Ntl_ManRootModel( p );
    if ( Ntl_ModelBoxNum(pModel) == 0 )
        return;
    if ( pModel->vResetFlops )
    {
        printf( "RESET STATISTICS:\n" );
        Vec_VecForEachLevel( pModel->vResetFlops, vFlops, i )
        {
            pNet = (Ntl_Net_t *)Vec_PtrEntry( pModel->vResets, i );
            printf( "Reset %2d :  Name = %30s   Flops = %6d.\n", i+1, pNet->pName, Vec_PtrSize(vFlops) );
            if ( i == 10 )
            {
                printf( "Skipping... (the total is %d)\n", Vec_VecSize(pModel->vResetFlops) );
                break;
            }
        }
    }
//    printf( "\n" );
}

/**Function*************************************************************

  Synopsis    [Allocates the model.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Mod_t * Ntl_ModelAlloc( Ntl_Man_t * pMan, char * pName )
{
    Ntl_Mod_t * p;
    // start the manager
    p = ABC_ALLOC( Ntl_Mod_t, 1 );
    memset( p, 0, sizeof(Ntl_Mod_t) );
    p->attrBox     = 1;
    p->attrComb    = 1;
    p->attrWhite   = 1;
    p->attrKeep    = 0;
    p->attrNoMerge = 0;
    p->pMan  = pMan;
    p->pName = Ntl_ManStoreName( p->pMan, pName );
    p->vObjs = Vec_PtrAlloc( 100 );
    p->vPis  = Vec_PtrAlloc( 10 );
    p->vPos  = Vec_PtrAlloc( 10 );
    p->vNets = Vec_PtrAlloc( 100 );
    // start the table
    p->nTableSize = Aig_PrimeCudd( 100 );
    p->pTable = ABC_ALLOC( Ntl_Net_t *, p->nTableSize );
    memset( p->pTable, 0, sizeof(Ntl_Net_t *) * p->nTableSize );
    // add model to the table
    if ( !Ntl_ManAddModel( pMan, p ) )
    {
        Ntl_ModelFree( p );
        return NULL;
    }
    return p;
}

/**Function*************************************************************

  Synopsis    [Duplicates the model without nodes but with CI/CO nets.]

  Description [The CI/CO nets of the old model should be marked before
  calling this procedure.]

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Mod_t * Ntl_ModelStartFrom( Ntl_Man_t * pManNew, Ntl_Mod_t * pModelOld )
{
    Ntl_Mod_t * pModelNew;
    Ntl_Net_t * pNet;
    Ntl_Obj_t * pObj;
    int i, k;
    pModelNew = Ntl_ModelAlloc( pManNew, pModelOld->pName );
    pModelNew->attrWhite   = pModelOld->attrWhite;
	pModelNew->attrBox	   = pModelOld->attrBox;
	pModelNew->attrComb	   = pModelOld->attrComb;
	pModelNew->attrKeep	   = pModelOld->attrKeep;
	pModelNew->attrNoMerge = pModelOld->attrNoMerge;
    Ntl_ModelForEachObj( pModelOld, pObj, i )
    {
        if ( Ntl_ObjIsNode(pObj) )
            pObj->pCopy = NULL;
        else
            pObj->pCopy = Ntl_ModelDupObj( pModelNew, pObj );
    }
    Ntl_ModelForEachNet( pModelOld, pNet, i )
    {
        if ( pNet->pDriver == NULL )
            pNet->pCopy = Ntl_ModelFindOrCreateNet( pModelNew, pNet->pName );
        else if ( pNet->fMark )
        {
            pNet->pCopy = Ntl_ModelFindOrCreateNet( pModelNew, pNet->pName );
            ((Ntl_Net_t *)pNet->pCopy)->pDriver = (Ntl_Obj_t *)pNet->pDriver->pCopy;
        }
        else
            pNet->pCopy = NULL;
        if ( pNet->pCopy )
            ((Ntl_Net_t *)pNet->pCopy)->fFixed = pNet->fFixed;
    }
    Ntl_ModelForEachObj( pModelOld, pObj, i )
    {
        if ( Ntl_ObjIsNode(pObj) )
            continue;
        Ntl_ObjForEachFanin( pObj, pNet, k )
            if ( pNet->pCopy != NULL )
                Ntl_ObjSetFanin( (Ntl_Obj_t *)pObj->pCopy, (Ntl_Net_t *)pNet->pCopy, k );
        Ntl_ObjForEachFanout( pObj, pNet, k )
            if ( pNet->pCopy != NULL )
                Ntl_ObjSetFanout( (Ntl_Obj_t *)pObj->pCopy, (Ntl_Net_t *)pNet->pCopy, k );
        if ( Ntl_ObjIsLatch(pObj) )
        {
            ((Ntl_Obj_t *)pObj->pCopy)->LatchId = pObj->LatchId;
            ((Ntl_Obj_t *)pObj->pCopy)->pClock = (Ntl_Net_t *)pObj->pClock->pCopy;
        }
    }
    pModelNew->vDelays = pModelOld->vDelays? Vec_IntDup( pModelOld->vDelays ) : NULL;
    pModelNew->vTimeInputs = pModelOld->vTimeInputs? Vec_IntDup( pModelOld->vTimeInputs ) : NULL;
    pModelNew->vTimeOutputs = pModelOld->vTimeOutputs? Vec_IntDup( pModelOld->vTimeOutputs ) : NULL;
    return pModelNew;
}

/**Function*************************************************************

  Synopsis    [Duplicates the model.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Mod_t * Ntl_ModelDup( Ntl_Man_t * pManNew, Ntl_Mod_t * pModelOld )
{
    Ntl_Mod_t * pModelNew;
    Ntl_Net_t * pNet;
    Ntl_Obj_t * pObj;
    int i, k;
    pModelNew = Ntl_ModelAlloc( pManNew, pModelOld->pName );
    pModelNew->attrWhite   = pModelOld->attrWhite;
	pModelNew->attrBox	   = pModelOld->attrBox;
	pModelNew->attrComb	   = pModelOld->attrComb;
	pModelNew->attrKeep	   = pModelOld->attrKeep;
	pModelNew->attrNoMerge = pModelOld->attrNoMerge;
    Ntl_ModelForEachObj( pModelOld, pObj, i )
        pObj->pCopy = Ntl_ModelDupObj( pModelNew, pObj );
    Ntl_ModelForEachNet( pModelOld, pNet, i )
    {
        pNet->pCopy = Ntl_ModelFindOrCreateNet( pModelNew, pNet->pName );
        ((Ntl_Net_t *)pNet->pCopy)->fFixed = pNet->fFixed;
        if ( pNet->pDriver == NULL )
        {
            assert( !pModelOld->attrWhite );
            continue;
        }
        ((Ntl_Net_t *)pNet->pCopy)->pDriver = (Ntl_Obj_t *)pNet->pDriver->pCopy;
        assert( pNet->pDriver->pCopy != NULL );
    }
    Ntl_ModelForEachObj( pModelOld, pObj, i )
    {
        Ntl_ObjForEachFanin( pObj, pNet, k )
            Ntl_ObjSetFanin( (Ntl_Obj_t *)pObj->pCopy, (Ntl_Net_t *)pNet->pCopy, k );
        Ntl_ObjForEachFanout( pObj, pNet, k )
            Ntl_ObjSetFanout( (Ntl_Obj_t *)pObj->pCopy, (Ntl_Net_t *)pNet->pCopy, k );
        if ( Ntl_ObjIsLatch(pObj) )
        {
            ((Ntl_Obj_t *)pObj->pCopy)->LatchId = pObj->LatchId;
            ((Ntl_Obj_t *)pObj->pCopy)->pClock = pObj->pClock? (Ntl_Net_t *)pObj->pClock->pCopy : NULL;
        }
        if ( Ntl_ObjIsNode(pObj) )
            ((Ntl_Obj_t *)pObj->pCopy)->pSop = Ntl_ManStoreSop( pManNew->pMemSops, pObj->pSop );
    }
    pModelNew->vDelays = pModelOld->vDelays? Vec_IntDup( pModelOld->vDelays ) : NULL;
    pModelNew->vTimeInputs = pModelOld->vTimeInputs? Vec_IntDup( pModelOld->vTimeInputs ) : NULL;
    pModelNew->vTimeOutputs = pModelOld->vTimeOutputs? Vec_IntDup( pModelOld->vTimeOutputs ) : NULL;
    return pModelNew;
}


// *r x\large\club_u2.blif.bz2;     *ps; *clplut; *ps
// *r x\large\amazon_core.blif.bz2; *ps; *clplut; *ps


/**Function*************************************************************

  Synopsis    [Duplicates the model.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Mod_t * Ntl_ModelDupCollapseLuts( Ntl_Man_t * pManNew, Ntl_Mod_t * pModelOld )
{
    Ntl_Mod_t * pModelNew, * pModelBox;
    Ntl_Net_t * pNet;
    Ntl_Obj_t * pObj, * pObjBox;
    char * pNameBuf = ABC_ALLOC( char, 10000 );
    int i, k, m, Counter = 0;
    pModelNew = Ntl_ModelAlloc( pManNew, pModelOld->pName );
    pModelNew->attrWhite   = pModelOld->attrWhite;
	pModelNew->attrBox	   = pModelOld->attrBox;
	pModelNew->attrComb	   = pModelOld->attrComb;
	pModelNew->attrKeep	   = pModelOld->attrKeep;
	pModelNew->attrNoMerge = pModelOld->attrNoMerge;
    Ntl_ModelForEachObj( pModelOld, pObj, i )
        if ( Ntl_ObjIsLutBox(pObj) ) // skip collapsible LUT boxes
            pObj->pCopy = NULL;
        else
            pObj->pCopy = Ntl_ModelDupObj( pModelNew, pObj );
    Ntl_ModelForEachNet( pModelOld, pNet, i )
    {
        pNet->pCopy = Ntl_ModelFindOrCreateNet( pModelNew, pNet->pName );
        ((Ntl_Net_t *)pNet->pCopy)->fFixed = pNet->fFixed;
        if ( pNet->pDriver == NULL )
        {
            assert( !pModelOld->attrWhite );
            continue;
        }
        if ( Ntl_ObjIsLutBox(pNet->pDriver) )
            continue;
        ((Ntl_Net_t *)pNet->pCopy)->pDriver = (Ntl_Obj_t *)pNet->pDriver->pCopy;
        assert( pNet->pDriver->pCopy != NULL );
    }
    Ntl_ModelForEachObj( pModelOld, pObj, i )
    {
        if ( Ntl_ObjIsLutBox(pObj) ) // collapse LUT boxes
        {
            pModelBox = pObj->pImplem;
            assert( pModelBox->attrComb );
            assert( Ntl_ObjFaninNum(pObj) == Ntl_ModelPiNum(pModelBox) );
            assert( Ntl_ObjFanoutNum(pObj) == Ntl_ModelPoNum(pModelBox) );
            Ntl_ModelClearNets( pModelBox );
            // attach PI/PO nets
            Ntl_ModelForEachPi( pModelBox, pObjBox, k )
                Ntl_ObjFanout0(pObjBox)->pCopy = Ntl_ObjFanin(pObj, k)->pCopy;
            Ntl_ModelForEachPo( pModelBox, pObjBox, k )
                Ntl_ObjFanin0(pObjBox)->pCopy = Ntl_ObjFanout(pObj, k)->pCopy;
            // duplicate internal nodes
            Ntl_ModelForEachNode( pModelBox, pObjBox, k )
                pObjBox->pCopy = Ntl_ModelDupObj( pModelNew, pObjBox );
            // duplicate and connect nets
            Ntl_ModelForEachNet( pModelBox, pNet, k )
            {
                if ( pNet->pCopy != NULL )
                    continue;
                sprintf( pNameBuf, "box%d_%s", i, pNet->pName );
                pNet->pCopy = Ntl_ModelFindOrCreateNet( pModelNew, pNameBuf ); // change name!!!
                ((Ntl_Net_t *)pNet->pCopy)->pDriver = (Ntl_Obj_t *)pNet->pDriver->pCopy;
            }
            // connect nodes
            Ntl_ModelForEachNode( pModelBox, pObjBox, k )
            {
                Ntl_ObjForEachFanin( pObjBox, pNet, m )
                    Ntl_ObjSetFanin( (Ntl_Obj_t *)pObjBox->pCopy, (Ntl_Net_t *)pNet->pCopy, m );
                Ntl_ObjForEachFanout( pObjBox, pNet, m )
                    Ntl_ObjSetFanout( (Ntl_Obj_t *)pObjBox->pCopy, (Ntl_Net_t *)pNet->pCopy, m );
                ((Ntl_Obj_t *)pObjBox->pCopy)->pSop = Ntl_ManStoreSop( pManNew->pMemSops, pObjBox->pSop );
            }
            // connect the PO nets
            Ntl_ModelForEachPo( pModelBox, pObjBox, k )
                ((Ntl_Net_t *)Ntl_ObjFanin0(pObjBox)->pCopy)->pDriver = (Ntl_Obj_t *)Ntl_ObjFanin0(pObjBox)->pDriver->pCopy;
            assert( pObj->pCopy == NULL );
            Counter++;
        }
        else
        {
            Ntl_ObjForEachFanin( pObj, pNet, k )
                Ntl_ObjSetFanin( (Ntl_Obj_t *)pObj->pCopy, (Ntl_Net_t *)pNet->pCopy, k );
            Ntl_ObjForEachFanout( pObj, pNet, k )
                Ntl_ObjSetFanout( (Ntl_Obj_t *)pObj->pCopy, (Ntl_Net_t *)pNet->pCopy, k );
            if ( Ntl_ObjIsLatch(pObj) )
            {
                ((Ntl_Obj_t *)pObj->pCopy)->LatchId = pObj->LatchId;
                ((Ntl_Obj_t *)pObj->pCopy)->pClock = pObj->pClock? (Ntl_Net_t *)pObj->pClock->pCopy : NULL;
            }
            if ( Ntl_ObjIsNode(pObj) )
                ((Ntl_Obj_t *)pObj->pCopy)->pSop = Ntl_ManStoreSop( pManNew->pMemSops, pObj->pSop );
        }
    }
    pModelNew->vDelays = pModelOld->vDelays? Vec_IntDup( pModelOld->vDelays ) : NULL;
    pModelNew->vTimeInputs = pModelOld->vTimeInputs? Vec_IntDup( pModelOld->vTimeInputs ) : NULL;
    pModelNew->vTimeOutputs = pModelOld->vTimeOutputs? Vec_IntDup( pModelOld->vTimeOutputs ) : NULL;
    ABC_FREE( pNameBuf );
    if ( Counter )
    printf( "Collapsed %d LUT boxes.\n", Counter );
    return pModelNew;
}

/**Function*************************************************************

  Synopsis    [Deallocates the model.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Ntl_ModelFree( Ntl_Mod_t * p )
{
    assert( Ntl_ModelCheckNetsAreNotMarked(p) );
    if ( p->vTimeOutputs )  Vec_IntFree( p->vTimeOutputs );
    if ( p->vTimeInputs )   Vec_IntFree( p->vTimeInputs );
    if ( p->vDelays )       Vec_IntFree( p->vDelays );
    if ( p->vClocks )       Vec_PtrFree( p->vClocks );
    if ( p->vClockFlops )   Vec_VecFree( p->vClockFlops );
    if ( p->vResets )       Vec_PtrFree( p->vResets );
    if ( p->vResetFlops )   Vec_VecFree( p->vResetFlops );
    Vec_PtrFree( p->vNets );
    Vec_PtrFree( p->vObjs );
    Vec_PtrFree( p->vPis );
    Vec_PtrFree( p->vPos );
    ABC_FREE( p->pTable );
    ABC_FREE( p );
}

/**Function*************************************************************

  Synopsis    [Create model equal to the latch with the given init value.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
Ntl_Mod_t * Ntl_ManCreateLatchModel( Ntl_Man_t * pMan, int Init )
{
    char Name[100];
    Ntl_Mod_t * pModel;
    Ntl_Obj_t * pObj;
    Ntl_Net_t * pNetLi, * pNetLo;
    // create model
    sprintf( Name, "%s%d", "latch", Init );
    pModel = Ntl_ModelAlloc( pMan, Name );
    pModel->attrWhite   = 1;
    pModel->attrBox     = 1;
    pModel->attrComb    = 0;
    pModel->attrKeep    = 0;
    pModel->attrNoMerge = 0;
    // create primary input
    pObj = Ntl_ModelCreatePi( pModel );
    pNetLi = Ntl_ModelFindOrCreateNet( pModel, "li" );
    Ntl_ModelSetNetDriver( pObj, pNetLi );
    // create latch
    pObj = Ntl_ModelCreateLatch( pModel );
    pObj->LatchId.regInit = Init;
    pObj->pFanio[0] = pNetLi;
    // create primary output
    pNetLo = Ntl_ModelFindOrCreateNet( pModel, "lo" );
    Ntl_ModelSetNetDriver( pObj, pNetLo );
    pObj = Ntl_ModelCreatePo( pModel, pNetLo );
    // set timing information
    pModel->vTimeInputs = Vec_IntAlloc( 2 );
    Vec_IntPush( pModel->vTimeInputs, -1 );
    Vec_IntPush( pModel->vTimeInputs, Aig_Float2Int(0.0) );
    pModel->vTimeOutputs = Vec_IntAlloc( 2 );
    Vec_IntPush( pModel->vTimeOutputs, -1 );
    Vec_IntPush( pModel->vTimeOutputs, Aig_Float2Int(0.0) );
    return pModel;
}


/**Function*************************************************************

  Synopsis    [Count constant nodes.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ModelCountLut0( Ntl_Mod_t * p )
{
    Ntl_Obj_t * pNode;
    int i, Counter = 0;
    Ntl_ModelForEachNode( p, pNode, i )
        if ( Ntl_ObjFaninNum(pNode) == 0 )
            Counter++;
    return Counter;
}

/**Function*************************************************************

  Synopsis    [Count single-output nodes.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ModelCountLut1( Ntl_Mod_t * p )
{
    Ntl_Obj_t * pNode;
    int i, Counter = 0;
    Ntl_ModelForEachNode( p, pNode, i )
        if ( Ntl_ObjFaninNum(pNode) == 1 )
            Counter++;
    return Counter;
}

/**Function*************************************************************

  Synopsis    [Count buffers]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ModelCountBuf( Ntl_Mod_t * p )
{
    Ntl_Obj_t * pNode;
    int i, Counter = 0;
    Ntl_ModelForEachNode( p, pNode, i )
        if ( Ntl_ObjFaninNum(pNode) == 1 && pNode->pSop[0] == '1' )
            Counter++;
    return Counter;
}

/**Function*************************************************************

  Synopsis    [Count inverters.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Ntl_ModelCountInv( Ntl_Mod_t * p )
{
    Ntl_Obj_t * pNode;
    int i, Counter = 0;
    Ntl_ModelForEachNode( p, pNode, i )
        if ( Ntl_ObjFaninNum(pNode) == 1 && pNode->pSop[0] == '0' )
            Counter++;
    return Counter;
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END
