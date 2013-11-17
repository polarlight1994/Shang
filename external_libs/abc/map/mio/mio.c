/**CFile****************************************************************

  FileName    [mio.c]

  PackageName [MVSIS 1.3: Multi-valued logic synthesis system.]

  Synopsis    [File reading/writing for technology mapping.]

  Author      [MVSIS Group]

  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - August 18, 2003.]

  Revision    [$Id: mio.c,v 1.4 2004/08/05 18:34:51 satrajit Exp $]

***********************************************************************/

#define _BSD_SOURCE

#ifndef WIN32
#include <unistd.h>
#endif

#include "main.h"
#include "mio.h"
#include "mapper.h"
#include "amap.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

static int Mio_CommandReadLiberty( Abc_Frame_t * pAbc, int argc, char **argv );

static int Mio_CommandReadLibrary( Abc_Frame_t * pAbc, int argc, char **argv );
static int Mio_CommandPrintLibrary( Abc_Frame_t * pAbc, int argc, char **argv );

static int Mio_CommandReadLibrary2( Abc_Frame_t * pAbc, int argc, char **argv );
static int Mio_CommandPrintLibrary2( Abc_Frame_t * pAbc, int argc, char **argv );

// internal version of GENLIB library
static char * pMcncGenlib[25] = {
    "GATE inv1    1   O=!a;             PIN * INV     1 999 0.9 0.0 0.9 0.0\n",
    "GATE inv2    2   O=!a;             PIN * INV     2 999 1.0 0.0 1.0 0.0\n",
    "GATE inv3    3   O=!a;             PIN * INV     3 999 1.1 0.0 1.1 0.0\n",
    "GATE inv4    4   O=!a;             PIN * INV     4 999 1.2 0.0 1.2 0.0\n",
    "GATE nand2   2   O=!(a*b);         PIN * INV     1 999 1.0 0.0 1.0 0.0\n",
    "GATE nand3   3   O=!(a*b*c);       PIN * INV     1 999 1.1 0.0 1.1 0.0\n",
    "GATE nand4   4   O=!(a*b*c*d);     PIN * INV     1 999 1.4 0.0 1.4 0.0\n",
    "GATE nor2    2   O=!(a+b);         PIN * INV     1 999 1.4 0.0 1.4 0.0\n",
    "GATE nor3    3   O=!(a+b+c);       PIN * INV     1 999 2.4 0.0 2.4 0.0\n",
    "GATE nor4    4   O=!(a+b+c+d);     PIN * INV     1 999 3.8 0.0 3.8 0.0\n",
    "GATE xora    5   O=a*!b+!a*b;      PIN * UNKNOWN 2 999 1.9 0.0 1.9 0.0\n",
    "GATE xorb    5   O=!(a*b+!a*!b);   PIN * UNKNOWN 2 999 1.9 0.0 1.9 0.0\n",
    "GATE xnora   5   O=a*b+!a*!b;      PIN * UNKNOWN 2 999 2.1 0.0 2.1 0.0\n",
    "GATE xnorb   5   O=!(!a*b+a*!b);   PIN * UNKNOWN 2 999 2.1 0.0 2.1 0.0\n",
    "GATE aoi21   3   O=!(a*b+c);       PIN * INV     1 999 1.6 0.0 1.6 0.0\n",
    "GATE aoi22   4   O=!(a*b+c*d);     PIN * INV     1 999 2.0 0.0 2.0 0.0\n",
    "GATE oai21   3   O=!((a+b)*c);     PIN * INV     1 999 1.6 0.0 1.6 0.0\n",
    "GATE oai22   4   O=!((a+b)*(c+d)); PIN * INV     1 999 2.0 0.0 2.0 0.0\n",
    "GATE buf     1   O=a;              PIN * NONINV  1 999 1.0 0.0 1.0 0.0\n",
    "GATE zero    0   O=CONST0;\n",
    "GATE one     0   O=CONST1;\n"
};

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Mio_Init( Abc_Frame_t * pAbc )
{
/*
    char * pFileTemp = "mcnc_temp.genlib";
    void * pLibGen;
    FILE * pFile;
    int i;

    // write genlib into file
    pFile = fopen( pFileTemp, "w" );
    for ( i = 0; pMcncGenlib[i]; i++ )
        fputs( pMcncGenlib[i], pFile );
    fclose( pFile );
    // read genlib from file
    pLibGen = Mio_LibraryRead( pAbc, pFileTemp, NULL, 0 );
    Abc_FrameSetLibGen( pLibGen );
    pLibGen = Amap_LibReadAndPrepare( pFileTemp, 0, 0 );
    Abc_FrameSetLibGen2( pLibGen );

#ifdef WIN32
        _unlink( pFileTemp );
#else
        unlink( pFileTemp );
#endif
*/

    Cmd_CommandAdd( pAbc, "SC mapping", "read_liberty",   Mio_CommandReadLiberty,  0 );

    Cmd_CommandAdd( pAbc, "SC mapping", "read_library",   Mio_CommandReadLibrary,  0 );
    Cmd_CommandAdd( pAbc, "SC mapping", "print_library",  Mio_CommandPrintLibrary, 0 );

    Cmd_CommandAdd( pAbc, "SC mapping", "read_library2",   Mio_CommandReadLibrary2,  0 );
    Cmd_CommandAdd( pAbc, "SC mapping", "print_library2",  Mio_CommandPrintLibrary2, 0 );
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
void Mio_End( Abc_Frame_t * pAbc )
{
//    Mio_LibraryDelete( s_pLib );
    Mio_LibraryDelete( (Mio_Library_t *)Abc_FrameReadLibGen() );
    Amap_LibFree( (Amap_Lib_t *)Abc_FrameReadLibGen2() );
}


/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Mio_CommandReadLiberty( Abc_Frame_t * pAbc, int argc, char **argv )
{
    FILE * pFile;
    FILE * pOut, * pErr;
    Abc_Ntk_t * pNet;
    char * FileName;
    int fVerbose;
    int c;

    pNet = Abc_FrameReadNtk(pAbc);
    pOut = Abc_FrameReadOut(pAbc);
    pErr = Abc_FrameReadErr(pAbc);

    // set the defaults
    fVerbose = 1;
    Extra_UtilGetoptReset();
    while ( (c = Extra_UtilGetopt(argc, argv, "vh")) != EOF )
    {
        switch (c)
        {
            case 'v':
                fVerbose ^= 1;
                break;
            case 'h':
                goto usage;
                break;
            default:
                goto usage;
        }
    }


    if ( argc != globalUtilOptind + 1 )
    {
        goto usage;
    }

    // get the input file name
    FileName = argv[globalUtilOptind];
    if ( (pFile = Io_FileOpen( FileName, "open_path", "r", 0 )) == NULL )
    {
        fprintf( pErr, "Cannot open input file \"%s\". ", FileName );
        if ( (FileName = Extra_FileGetSimilarName( FileName, ".genlib", ".lib", ".gen", ".g", NULL )) )
            fprintf( pErr, "Did you mean \"%s\"?", FileName );
        fprintf( pErr, "\n" );
        return 1;
    }
    fclose( pFile );

    if ( !Amap_LibertyParse( FileName, "temp.genlib", fVerbose ) )
        return 0;
    Cmd_CommandExecute( pAbc, "read_library temp.genlib" );
    return 0;

usage:
    fprintf( pErr, "usage: read_liberty [-vh]\n");
    fprintf( pErr, "\t         read standard cell library in Liberty format\n" );
    fprintf( pErr, "\t         (if the library contains more than one gate\n" );
    fprintf( pErr, "\t         with the same Boolean function, only the gate\n" );
    fprintf( pErr, "\t         with the smallest area will be used)\n" );
    fprintf( pErr, "\t-v     : toggle verbose printout [default = %s]\n", fVerbose? "yes": "no" );
    fprintf( pErr, "\t-h     : enable verbose output\n");
    return 1;       /* error exit */
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Mio_CommandReadLibrary( Abc_Frame_t * pAbc, int argc, char **argv )
{
    FILE * pFile;
    FILE * pOut, * pErr;
    Mio_Library_t * pLib;
    Abc_Ntk_t * pNet;
    char * FileName;
    int fVerbose;
    int c;

    pNet = Abc_FrameReadNtk(pAbc);
    pOut = Abc_FrameReadOut(pAbc);
    pErr = Abc_FrameReadErr(pAbc);

    // set the defaults
    fVerbose = 1;
    Extra_UtilGetoptReset();
    while ( (c = Extra_UtilGetopt(argc, argv, "vh")) != EOF )
    {
        switch (c)
        {
            case 'v':
                fVerbose ^= 1;
                break;
            case 'h':
                goto usage;
                break;
            default:
                goto usage;
        }
    }


    if ( argc != globalUtilOptind + 1 )
    {
        goto usage;
    }

    // get the input file name
    FileName = argv[globalUtilOptind];
    if ( (pFile = Io_FileOpen( FileName, "open_path", "r", 0 )) == NULL )
    {
        fprintf( pErr, "Cannot open input file \"%s\". ", FileName );
        if ( (FileName = Extra_FileGetSimilarName( FileName, ".genlib", ".lib", ".gen", ".g", NULL )) )
            fprintf( pErr, "Did you mean \"%s\"?", FileName );
        fprintf( pErr, "\n" );
        return 1;
    }
    fclose( pFile );

    // set the new network
    pLib = Mio_LibraryRead( FileName, 0, fVerbose );
    if ( pLib == NULL )
    {
        fprintf( pErr, "Reading GENLIB library has failed.\n" );
        return 1;
    }
    // free the current superlib because it depends on the old Mio library
    if ( Abc_FrameReadLibSuper() )
    {
        Map_SuperLibFree( (Map_SuperLib_t *)Abc_FrameReadLibSuper() );
        Abc_FrameSetLibSuper( NULL );
    }

    // replace the current library
    Mio_LibraryDelete( (Mio_Library_t *)Abc_FrameReadLibGen() );
    Abc_FrameSetLibGen( pLib );

    // set the new network
    pLib = (Mio_Library_t *)Amap_LibReadAndPrepare( FileName, 1, 0 );
    if ( pLib == NULL )
    {
        fprintf( pErr, "Reading GENLIB library has failed.\n" );
        return 1;
    }
    // replace the current library
    Amap_LibFree( (Amap_Lib_t *)Abc_FrameReadLibGen2() );
    Abc_FrameSetLibGen2( pLib );
    return 0;

usage:
    fprintf( pErr, "usage: read_library [-vh]\n");
    fprintf( pErr, "\t         read the library from a genlib file\n" );
    fprintf( pErr, "\t         (if the library contains more than one gate\n" );
    fprintf( pErr, "\t         with the same Boolean function, only the gate\n" );
    fprintf( pErr, "\t         with the smallest area will be used)\n" );
    fprintf( pErr, "\t-v     : toggle verbose printout [default = %s]\n", fVerbose? "yes": "no" );
    fprintf( pErr, "\t-h     : enable verbose output\n");
    return 1;       /* error exit */
}

/**Function*************************************************************

  Synopsis    []

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Mio_CommandReadLibrary2( Abc_Frame_t * pAbc, int argc, char **argv )
{
    FILE * pFile;
    FILE * pOut, * pErr;
    Mio_Library_t * pLib;
    Abc_Ntk_t * pNet;
    char * FileName;
    int fVerbose;
    int fVeryVerbose;
    int c;

    pNet = Abc_FrameReadNtk(pAbc);
    pOut = Abc_FrameReadOut(pAbc);
    pErr = Abc_FrameReadErr(pAbc);

    // set the defaults
    fVerbose     = 1;
    fVeryVerbose = 0;
    Extra_UtilGetoptReset();
    while ( (c = Extra_UtilGetopt(argc, argv, "vwh")) != EOF )
    {
        switch (c)
        {
            case 'v':
                fVerbose ^= 1;
                break;
            case 'w':
                fVeryVerbose ^= 1;
                break;
            case 'h':
                goto usage;
                break;
            default:
                goto usage;
        }
    }


    if ( argc != globalUtilOptind + 1 )
    {
        goto usage;
    }

    // get the input file name
    FileName = argv[globalUtilOptind];
    if ( (pFile = Io_FileOpen( FileName, "open_path", "r", 0 )) == NULL )
    {
        fprintf( pErr, "Cannot open input file \"%s\". ", FileName );
        if ( (FileName = Extra_FileGetSimilarName( FileName, ".genlib", ".lib", ".gen", ".g", NULL )) )
            fprintf( pErr, "Did you mean \"%s\"?", FileName );
        fprintf( pErr, "\n" );
        return 1;
    }
    fclose( pFile );

    // set the new network
    pLib = (Mio_Library_t *)Amap_LibReadAndPrepare( FileName, fVerbose, fVeryVerbose );
    if ( pLib == NULL )
    {
        fprintf( pErr, "Reading GENLIB library has failed.\n" );
        return 1;
    }

    // replace the current library
    Amap_LibFree( (Amap_Lib_t *)Abc_FrameReadLibGen2() );
    Abc_FrameSetLibGen2( pLib );
    return 0;

usage:
    fprintf( pErr, "usage: read_library2 [-vh]\n");
    fprintf( pErr, "\t         read the library from a genlib file\n" );
    fprintf( pErr, "\t         (if the library contains more than one gate\n" );
    fprintf( pErr, "\t         with the same Boolean function, only the gate\n" );
    fprintf( pErr, "\t         with the smallest area will be used)\n" );
    fprintf( pErr, "\t-v     : toggle verbose printout [default = %s]\n", fVerbose? "yes": "no" );
    fprintf( pErr, "\t-w     : toggle detailed printout [default = %s]\n", fVeryVerbose? "yes": "no" );
    fprintf( pErr, "\t-h     : enable verbose output\n");
    return 1;       /* error exit */
}


/**Function*************************************************************

  Synopsis    [Command procedure to read LUT libraries.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Mio_CommandPrintLibrary( Abc_Frame_t * pAbc, int argc, char **argv )
{
    FILE * pOut, * pErr;
    Abc_Ntk_t * pNet;
    int fVerbose;
    int c;

    pNet = Abc_FrameReadNtk(pAbc);
    pOut = Abc_FrameReadOut(pAbc);
    pErr = Abc_FrameReadErr(pAbc);

    // set the defaults
    fVerbose = 1;
    Extra_UtilGetoptReset();
    while ( (c = Extra_UtilGetopt(argc, argv, "vh")) != EOF )
    {
        switch (c)
        {
            case 'v':
                fVerbose ^= 1;
                break;
            case 'h':
                goto usage;
                break;
            default:
                goto usage;
        }
    }


    if ( argc != globalUtilOptind )
    {
        goto usage;
    }

    // set the new network
    Mio_WriteLibrary( stdout, (Mio_Library_t *)Abc_FrameReadLibGen(), 0 );
    return 0;

usage:
    fprintf( pErr, "\nusage: print_library [-vh]\n");
    fprintf( pErr, "\t          print the current genlib library\n" );
    fprintf( pErr, "\t-v      : toggles enabling of verbose output [default = %s]\n", (fVerbose? "yes" : "no") );
    fprintf( pErr, "\t-h      : print the command usage\n");
    return 1;       /* error exit */
}


/**Function*************************************************************

  Synopsis    [Command procedure to read LUT libraries.]

  Description []

  SideEffects []

  SeeAlso     []

***********************************************************************/
int Mio_CommandPrintLibrary2( Abc_Frame_t * pAbc, int argc, char **argv )
{
    FILE * pOut, * pErr;
    Abc_Ntk_t * pNet;
    int fPrintAll;
    int fVerbose;
    int c;

    pNet = Abc_FrameReadNtk(pAbc);
    pOut = Abc_FrameReadOut(pAbc);
    pErr = Abc_FrameReadErr(pAbc);

    // set the defaults
    fPrintAll = 0;
    fVerbose = 1;
    Extra_UtilGetoptReset();
    while ( (c = Extra_UtilGetopt(argc, argv, "avh")) != EOF )
    {
        switch (c)
        {
            case 'a':
                fPrintAll ^= 1;
                break;
            case 'v':
                fVerbose ^= 1;
                break;
            case 'h':
                goto usage;
                break;
            default:
                goto usage;
        }
    }


    if ( argc != globalUtilOptind )
    {
        goto usage;
    }

    // set the new network
    Amap_LibPrintSelectedGates( (Amap_Lib_t *)Abc_FrameReadLibGen2(), fPrintAll );
    return 0;

usage:
    fprintf( pErr, "\nusage: print_library2 [-avh]\n");
    fprintf( pErr, "\t          print gates used for area-oriented tech-mapping\n" );
    fprintf( pErr, "\t-a      : toggles printing all gates [default = %s]\n", (fPrintAll? "yes" : "no") );
    fprintf( pErr, "\t-v      : toggles enabling of verbose output [default = %s]\n", (fVerbose? "yes" : "no") );
    fprintf( pErr, "\t-h      : print the command usage\n");
    return 1;       /* error exit */
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END
