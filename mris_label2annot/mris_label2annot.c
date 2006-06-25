// $Id: mris_label2annot.c,v 1.2 2006/06/25 20:42:24 greve Exp $

/*
  BEGINHELP

Converts a set of surface labels to an annotation file.

--s subject

Name of FreeSurfer subject.

--h hemi

Hemisphere (lh or rh).

--ctab colortablefile

File that defines the structure names, their indices, and their
color. This must have the same format as that found in
$FREESUFER_HOME/FreeSurferColorLUT.txt.

--l labelfile1 <--l labelfile2 ...>

List of label files. The labels should be defined on the surface (eg,
with tksurfer). The first label will be mapped to index 1 in the color
table file. The next label will be mapped to index 2, etc. Verticies
that are not mapped to a label are assigned index 0. If --no-unknown 
is specified, then the first label is mapped to index 0, etc, and unhit 
vertices are not mapped. 

--a annotname

Name of the annotation to create. The actual file will be called
hemi.annotname.annot, and it will be created in subject/label.
If this file exists, then mris_label2annot exits immediately
with an error message. It is then up to the user to manually
delete this file (this is so annotations are not accidentally
deleted, which could be a huge inconvenience).

--nhits nhitsfile

Overlay file with the number of labels for each vertex. Ideally, each
vertex would have only one label, but there is not constraint that
forces this. If there is more than one label for a vertex, the vertex
will be assigned to the last label as specified on the cmd line. This
can then be loaded as an overlay in tksurfer (set fthresh to 1.5). This
is mainly good for debugging.

--no-unknown

Start label numbering at index 0 instead of index 1. Do not map unhit
vertices (ie, vertices without a label) to 0.

EXAMPLE:

mris_label2annot --s bert --h lh --ctab aparc.annot.ctab \\
  --a myaparc --l lh.unknown.label --l lh.bankssts.label \\
  --l lh.caudalanteriorcingulate.label --nhits nhits.mgh

This will create lh.myaparc.annot in bert/labels using the three 
labels specified. Any vertices that have multiple labels will then 
be stored in nhits.mgh (as a volume-encoded surface file). 

To view, run:

tksurfer bert lh inflated -overlay nhits.mgh -fthresh 1.5

Then File->Label->ImportAnnotation and select lh.myaparc.annot.

  ENDHELP
*/

/*
  BEGINUSAGE
  ENDUSAGE
*/


#include <stdio.h>
#include <stdlib.h>
#include <math.h>
double round(double x);
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "macros.h"
#include "utils.h"
#include "mrisurf.h"
#include "mrisutils.h"
#include "error.h"
#include "diag.h"
#include "mri.h"
#include "mri2.h"
#include "fio.h"
#include "version.h"
#include "label.h"
#include "matrix.h"
#include "annotation.h"
#include "fmriutils.h"
#include "cmdargs.h"
#include "fsglm.h"
#include "gsl/gsl_cdf.h"
#include "pdf.h"
#include "fsgdf.h"
#include "timer.h"
#include "matfile.h"
#include "volcluster.h"
#include "surfcluster.h"


static int  parse_commandline(int argc, char **argv);
static void check_options(void);
static void print_usage(void) ;
static void usage_exit(void);
static void print_help(void) ;
static void print_version(void) ;
static void dump_options(FILE *fp);
int main(int argc, char *argv[]) ;

static char vcid[] = "$Id: mris_label2annot.c,v 1.2 2006/06/25 20:42:24 greve Exp $";
char *Progname = NULL;
char *cmdline, cwd[2000];
int debug=0;
int checkoptsonly=0;
struct utsname uts;

char tmpstr[1000];
char *subject, *hemi, *SUBJECTS_DIR;
char *LabelFiles[1000];
int nlabels = 0;
char *CTabFile;
char *AnnotName, *AnnotPath;
MRIS *mris;
LABEL *label;
COLOR_TABLE *ctab = NULL;
MRI *nhits;
char *NHitsFile=NULL;
int MapUnhitToUnknown=1;

/*---------------------------------------------------------------*/
int main(int argc, char *argv[])
{
  int nargs, nthlabel, n, vtxno, ano, index, nunhit;

  nargs = handle_version_option (argc, argv, vcid, "$Name:  $");
  if (nargs && argc - nargs == 1) exit (0);
  argc -= nargs;
  cmdline = argv2cmdline(argc,argv);
  uname(&uts);
  getcwd(cwd,2000);

  Progname = argv[0] ;
  argc --;
  argv++;
  ErrorInit(NULL, NULL, NULL) ;
  DiagInit(NULL, NULL, NULL) ;
  if(argc == 0) usage_exit();
  parse_commandline(argc, argv);
  check_options();
  if(checkoptsonly) return(0);
  dump_options(stdout);

  SUBJECTS_DIR = getenv("SUBJECTS_DIR");
  if(SUBJECTS_DIR == NULL){
    printf("ERROR: SUBJECTS_DIR not defined in environment\n");
    exit(1);
  }

  // Make sure subject exists
  sprintf(tmpstr,"%s/%s",SUBJECTS_DIR,subject);
  if(!fio_IsDirectory(tmpstr)){
    printf("ERROR: cannot find %s\n",tmpstr);
    exit(1);
  }

  // Get path to annot, make sure it does not exist
  sprintf(tmpstr,"%s/%s/label/%s.%s.annot",SUBJECTS_DIR,subject,hemi,AnnotName);
  if(fio_FileExistsReadable(tmpstr)){
    printf("ERROR: %s already exists\n",tmpstr);
    exit(1);
  }
  AnnotPath = strcpyalloc(tmpstr);

  // Read the color table
  printf("Reading ctab %s\n",CTabFile);
  ctab = CTABreadASCII(CTabFile);
  if(ctab == NULL){
    printf("ERROR: reading %s\n",CTabFile);
    exit(1);
  }

  // Read the surf
  sprintf(tmpstr,"%s/%s/surf/%s.orig",SUBJECTS_DIR,subject,hemi);
  printf("Loading %s\n",tmpstr);
  mris = MRISread(tmpstr);
  if(mris == NULL) exit(1);

  // Set up color table
  set_atable_from_ctable(ctab);
  mris->ct = ctab;

  // Set up something to keep track of nhits
  nhits = MRIalloc(mris->nvertices,1,1,MRI_INT);

  // Go thru each label
  for(nthlabel = 0; nthlabel < nlabels; nthlabel ++){
    label = LabelRead(subject,LabelFiles[nthlabel]);
    if(label == NULL){
      printf("ERROR: reading %s\n",LabelFiles[nthlabel]);
      exit(1);
    }
    index = nthlabel;
    if(MapUnhitToUnknown) index ++;

    for(n = 0; n < label->n_points; n++){
      vtxno = label->lv[n].vno; 
      if(vtxno < 0 || vtxno > mris->nvertices){
	printf("ERROR: %s, n=%d, vertex %d out of range\n",
	       LabelFiles[nthlabel],n,vtxno);
	exit(1);
      }
      if(MRIgetVoxVal(nhits,vtxno,0,0,0) > 0){
	printf("WARNING: vertex %d maps to multiple lables\n",vtxno);
      }
      MRIsetVoxVal(nhits,vtxno,0,0,0,MRIgetVoxVal(nhits,vtxno,0,0,0)+1);
      ano = index_to_annotation(index);
      mris->vertices[vtxno].annotation = ano;
      //printf("%5d %2d %2d %s\n",vtxno,segid,ano,index_to_name(segid));
    } // label ponts
    LabelFree(&label);
  }// Label

  nunhit = 0;
  if(MapUnhitToUnknown){
    printf("Mapping unhit to unknown\n");
    for(vtxno = 0; vtxno < mris->nvertices; vtxno++){
      if(MRIgetVoxVal(nhits,vtxno,0,0,0) == 0){
	ano = index_to_annotation(0);
	mris->vertices[vtxno].annotation = ano;
	nunhit ++;
      }
    }
    printf("Found %d unhit vertices\n",nunhit);
  }

  printf("Writing annot to %s\n",AnnotPath);
  MRISwriteAnnotation(mris, AnnotPath);

  if(NHitsFile != NULL) MRIwrite(nhits,NHitsFile);

  return 0;
}
/* --------------------------------------------- */
static int parse_commandline(int argc, char **argv)
{
  int  nargc , nargsused;
  char **pargv, *option ;

  if(argc < 1) usage_exit();

  nargc   = argc;
  pargv = argv;
  while(nargc > 0){

    option = pargv[0];
    if(debug) printf("%d %s\n",nargc,option);
    nargc -= 1;
    pargv += 1;

    nargsused = 0;

    if (!strcmp(option, "--help"))  print_help() ;
    else if (!strcmp(option, "--version")) print_version() ;
    else if (!strcmp(option, "--debug"))   debug = 1;
    else if (!strcmp(option, "--checkopts"))   checkoptsonly = 1;
    else if (!strcmp(option, "--nocheckopts")) checkoptsonly = 0;
    else if (!strcmp(option, "--no-unknown")) MapUnhitToUnknown = 0;

    else if(!strcmp(option, "--s") || !strcmp(option, "--subject")){
      if(nargc < 1) CMDargNErr(option,1);
      subject = pargv[0];
      nargsused = 1;
    }
    else if(!strcmp(option, "--h") || !strcmp(option, "--hemi")){
      if(nargc < 1) CMDargNErr(option,1);
      hemi = pargv[0];
      nargsused = 1;
    }
    else if(!strcmp(option, "--ctab")){
      if(nargc < 1) CMDargNErr(option,1);
      if(!fio_FileExistsReadable(pargv[0])){
	printf("ERROR: cannot find or read %s\n",pargv[0]);
	exit(1);
      }
      CTabFile = pargv[0];
      nargsused = 1;
    }
    else if(!strcmp(option, "--l")){
      if(nargc < 1) CMDargNErr(option,1);
      if(!fio_FileExistsReadable(pargv[0])){
	printf("ERROR: cannot find or read %s\n",pargv[0]);
	exit(1);
      }
      LabelFiles[nlabels] = pargv[0];
      nlabels++;
      nargsused = 1;
    }
    else if(!strcmp(option, "--a") || !strcmp(option, "--annot")){
      if(nargc < 1) CMDargNErr(option,1);
      AnnotName = pargv[0];
      nargsused = 1;
    }
    else if(!strcmp(option, "--nhits")){
      if(nargc < 1) CMDargNErr(option,1);
      NHitsFile = pargv[0];
      nargsused = 1;
    }
    else{
      fprintf(stderr,"ERROR: Option %s unknown\n",option);
      if(CMDsingleDash(option))
	fprintf(stderr,"       Did you really mean -%s ?\n",option);
      exit(-1);
    }
    nargc -= nargsused;
    pargv += nargsused;
  }
  return(0);
}
/* ------------------------------------------------------ */
static void usage_exit(void)
{
  print_usage() ;
  exit(1) ;
}
/* --------------------------------------------- */
static void print_usage(void)
{
  printf("USAGE: %s \n",Progname) ;
  printf("\n");
  printf("   --s subject : FreeSurfer subject \n");
  printf("   --h hemi : hemisphere (lh or rh)\n");
  printf("   --ctab ctabfile : colortable (like FreeSurferColorLUT.txt)\n");
  printf("   --l label1 <--l label 2 ...> : label file(s)\n");
  printf("   --a annotname : output annotation file (hemi.annotname.annot)\n");
  printf("   --no-unknown : do not map unhit labels to index 0\n");
  printf("\n");
  printf("   --debug     turn on debugging\n");
  printf("   --checkopts don't run anything, just check options and exit\n");
  printf("   --help      print out information on how to use this program\n");
  printf("   --version   print out version and exit\n");
  printf("\n");
  printf("%s\n", vcid) ;
  printf("\n");
}
/* --------------------------------------------- */
static void print_help(void)
{
  print_usage() ;
printf("\n");
printf("Converts a set of surface labels to an annotation file.\n");
printf("\n");
printf("--s subject\n");
printf("\n");
printf("Name of FreeSurfer subject.\n");
printf("\n");
printf("--h hemi\n");
printf("\n");
printf("Hemisphere (lh or rh).\n");
printf("\n");
printf("--ctab colortablefile\n");
printf("\n");
printf("File that defines the structure names, their indices, and their\n");
printf("color. This must have the same format as that found in\n");
printf("$FREESUFER_HOME/FreeSurferColorLUT.txt.\n");
printf("\n");
printf("--l labelfile1 <--l labelfile2 ...>\n");
printf("\n");
printf("List of label files. The labels should be defined on the surface (eg,\n");
printf("with tksurfer). The first label will be mapped to index 1 in the color\n");
printf("table file. The next label will be mapped to index 2, etc. Verticies\n");
printf("that are not mapped to a label are assigned index 0. If --no-unknown \n");
printf("is specified, then the first label is mapped to index 0, etc, and unhit \n");
printf("vertices are not mapped. \n");
printf("\n");
printf("--a annotname\n");
printf("\n");
printf("Name of the annotation to create. The actual file will be called\n");
printf("hemi.annotname.annot, and it will be created in subject/label.\n");
printf("If this file exists, then mris_label2annot exits immediately\n");
printf("with an error message. It is then up to the user to manually\n");
printf("delete this file (this is so annotations are not accidentally\n");
printf("deleted, which could be a huge inconvenience).\n");
printf("\n");
printf("--nhits nhitsfile\n");
printf("\n");
printf("Overlay file with the number of labels for each vertex. Ideally, each\n");
printf("vertex would have only one label, but there is not constraint that\n");
printf("forces this. If there is more than one label for a vertex, the vertex\n");
printf("will be assigned to the last label as specified on the cmd line. This\n");
printf("can then be loaded as an overlay in tksurfer (set fthresh to 1.5). This\n");
printf("is mainly good for debugging.\n");
printf("\n");
printf("--no-unknown\n");
printf("\n");
printf("Start label numbering at index 0 instead of index 1. Do not map unhit\n");
printf("vertices (ie, vertices without a label) to 0.\n");
printf("\n");
printf("EXAMPLE:\n");
printf("\n");
printf("mris_label2annot --s bert --h lh --ctab aparc.annot.ctab \\\n");
printf("  --a myaparc --l lh.unknown.label --l lh.bankssts.label \\\n");
printf("  --l lh.caudalanteriorcingulate.label --nhits nhits.mgh\n");
printf("\n");
printf("This will create lh.myaparc.annot in bert/labels using the three \n");
printf("labels specified. Any vertices that have multiple labels will then \n");
printf("be stored in nhits.mgh (as a volume-encoded surface file). \n");
printf("\n");
printf("To view, run:\n");
printf("\n");
printf("tksurfer bert lh inflated -overlay nhits.mgh -fthresh 1.5\n");
printf("\n");
printf("Then File->Label->ImportAnnotation and select lh.myaparc.annot.\n");
printf("\n");

  exit(1) ;
}
/* --------------------------------------------- */
static void print_version(void)
{
  printf("%s\n", vcid) ;
  exit(1) ;
}
/* --------------------------------------------- */
static void check_options(void)
{
  if(subject == NULL){
    printf("ERROR: need to specify subject\n");
    exit(1);
  }
  if(hemi == NULL){
    printf("ERROR: need to specify hemi\n");
    exit(1);
  }
  if(nlabels == 0){
    printf("ERROR: no labels specified\n");
    exit(1);
  }
  if(CTabFile == NULL){
    printf("ERROR: need to specify color table file\n");
    exit(1);
  }

  if(AnnotName == NULL){
    printf("ERROR: need to specify annotation name\n");
    exit(1);
  }


  return;
}

/* --------------------------------------------- */
static void dump_options(FILE *fp)
{
  fprintf(fp,"\n");
  fprintf(fp,"%s\n",vcid);
  fprintf(fp,"cwd %s\n",cwd);
  fprintf(fp,"cmdline %s\n",cmdline);
  fprintf(fp,"sysname  %s\n",uts.sysname);
  fprintf(fp,"hostname %s\n",uts.nodename);
  fprintf(fp,"machine  %s\n",uts.machine);
  fprintf(fp,"user     %s\n",VERuser());
  fprintf(fp,"\n");
  fprintf(fp,"subject %s\n",subject);
  fprintf(fp,"hemi    %s\n",hemi);
  fprintf(fp,"SUBJECTS_DIR %s\n",SUBJECTS_DIR);
  fprintf(fp,"ColorTable %s\n",CTabFile);
  fprintf(fp,"AnnotName  %s\n",AnnotName);
  if(NHitsFile) fprintf(fp,"NHitsFile %s\n",NHitsFile);
  fprintf(fp,"nlables %d\n",nlabels);


  return;
}
