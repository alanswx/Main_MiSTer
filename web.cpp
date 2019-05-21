#include "web.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "scaler.h"
#include "lib/lodepng/lodepng.h"
#include "fpga_io.h"
#include "user_io.h"
#include "cheats.h"

#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <onion/onion.h>
#include <onion/handlers/exportlocal.h>
#include <onion/handlers/static.h>
#include <onion/dict.h>
#include <onion/shortcuts.h>
#include <onion/block.h>

#include <mister/richfilemanager.h>
#include <mister/uinput-key.h>

#ifdef __cplusplus
extern "C" {
#endif

int screenshot(void *p, onion_request * req, onion_response * res);
#ifdef __cplusplus
}
#endif


/**
 * @short Serves a directory listing.
 * 
 * It checks if the given request is a directory listing and processes it, or fallbacks to the
 * next handler.
 */

int filesearch(void *, onion_request * req, onion_response * res) {
  	const char *path=onion_request_get_queryd(req,"name","boot.rom");
  	//const char *fs_pFileExt=onion_request_get_queryd(req,"ext","rom");
  	//const char *output=onion_request_get_queryd(req,"output","json");

  onion_response_set_header(res, "Content-Type", "text/json");

    char *realp = realpath(path, NULL);
    if (!realp)
      return OCS_INTERNAL_ERROR;

    DIR *dir = opendir(realp);
    if (dir) {                  // its a dir, fill the dictionary.
      onion_dict *d = onion_dict_new();
      onion_dict_add(d, "dirname", path, 0);
      if (path[0] != '\0' && path[1] != '\0')
        onion_dict_add(d, "go_up", "true", 0);
      onion_dict *files = onion_dict_new();
      onion_dict_add(d, "files", files, OD_DICT | OD_FREE_VALUE);

      struct dirent *de;
      while ((de = readdir(dir))) {     // Fill one files.[filename] per file.
        onion_dict *file = onion_dict_new();
        onion_dict_add(files, de->d_name, file,
                       OD_DUP_KEY | OD_DICT | OD_FREE_VALUE);

        onion_dict_add(file, "name", de->d_name, OD_DUP_VALUE);

        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s/%s", realp, de->d_name);
        struct stat st;
        stat(tmp, &st);

        snprintf(tmp, sizeof(tmp), "%d", (int)st.st_size);
        onion_dict_add(file, "size", tmp, OD_DUP_VALUE);

        snprintf(tmp, sizeof(tmp), "%d", st.st_uid);
        onion_dict_add(file, "owner", tmp, OD_DUP_VALUE);

        if (S_ISDIR(st.st_mode))
          onion_dict_add(file, "type", "dir", 0);
        else
          onion_dict_add(file, "type", "file", 0);
      }
      closedir(dir);

      onion_block *jresb = onion_dict_to_json(d);
      onion_response_write(res, onion_block_data(jresb), onion_block_size(jresb));
    onion_block_free(jresb);
    onion_dict_free(d);

    }
    free(realp);
    return OCS_PROCESSED;
  }




int getScreenshotBuf(unsigned char **buf,unsigned int *outsize)
{
    mister_scaler *ms=mister_scaler_init();
    if (ms==NULL)
    {
        printf("problem with scaler, maybe not a new enough version\n");
        return -1;
    }
    else {
        unsigned char *outputbuf = (unsigned char *)calloc(ms->width*ms->height*3,1);
        mister_scaler_read(ms,outputbuf);
        unsigned error = lodepng_encode24(buf,outsize,outputbuf,ms->width,ms->height);

        if(error) {
        printf("error %u: %s\n", error, lodepng_error_text(error));
        return -1*error;
        }
        free(outputbuf);
        mister_scaler_free(ms);
    }

return 0;
}

int code = 0;

int keypress(void *, onion_request * req, onion_response * res) {
  onion_response_set_header(res, "Content-Type", "application/json");
  const char *keystr=onion_request_get_queryd(req,"key","");
  if (strlen(keystr)) {
	  uint16_t key = atoi(keystr);
	  printf("key: [%d]\n",key);
	  int newcode=keycode_to_key(key);
	  if (newcode==code){
          	user_io_kbd(code, 2);
	  } else if (code!=0) {
          	user_io_kbd(code, 0);
	  } else {
		code=newcode;
          	user_io_kbd(code, 1);
	  }

  	onion_response_write0(res, "{ \"result\" : \"success\" } ");
  }else
  {
  	onion_response_write0(res, "{ \"result\" : \"error\" } ");
  }

    return OCS_PROCESSED;
}
int screenshot(void *, onion_request * , onion_response * res) {
  unsigned char *buf;
  unsigned int outsize;
  onion_response_set_header(res, "Content-Type", "image/png");
  //if (onion_response_write_headers(res) == OR_SKIP_CONTENT)     // Maybe it was HEAD.
  //  return OCS_PROCESSED;

  int result = getScreenshotBuf(&buf,&outsize);
  fprintf(stdout,"screenshot: result: %d \n",result);
  if (result==0 && buf && outsize)
  {
  	ssize_t size= onion_response_write(res, (const char*)buf,outsize);
  	free(buf);
  	fprintf(stdout,"screenshot: result: %d size: %d\n",result,size);
    return OCS_PROCESSED;
  }
  else {
      return OCS_INTERNAL_ERROR;
  }

      return OCS_INTERNAL_ERROR;
}

int loadcore(void *, onion_request * req, onion_response * res) {
  fprintf(stderr,"loadcore\n");
  const char *core=onion_request_get_queryd(req,"name","menu.rbf");
  onion_response_write0(res, "done");
  onion_response_printf(res, "<p>name: %s",core);
  fprintf(stderr,"loadcore:%s\n",core);
  int result=fpga_load_rbf(core);
  fprintf(stderr,"loadcore:%s returning result: %d\n",core,result);
  return OCS_PROCESSED;

}

int getconfig(void *, onion_request * , onion_response * res) {
    int i=0;
    const char * p;
    onion_response_set_header(res, "Content-Type", "text/json");
    onion_response_printf(res, "[\n");
    do {
       p = user_io_8bit_get_string(i);
       printf("get cfgstring %d = %s\n", i, p);
       if (i!=0) onion_response_printf(res, " , ");
       onion_response_printf(res, "{\"%d\": \"%s\"}",i,p);
       i++;
    } while (p || i<3);
  onion_response_printf(res, "]\n");
  return OCS_PROCESSED;
}

int loadfile(void *, onion_request * req, onion_response * res) {

    int id = fpga_core_id();
    const char *SelectedPath=onion_request_get_queryd(req,"name","boot.rom");
    const char *fs_pFileExt=onion_request_get_queryd(req,"ext","rom");

    switch(id)
    {
        case CORE_TYPE_8BIT:
           printf("File selected: %s\n", SelectedPath);
           user_io_file_tx(SelectedPath, user_io_ext_idx((char *)SelectedPath, (char *)fs_pFileExt) << 6 | (0+ 1) );
           if(user_io_use_cheats()) cheats_init(SelectedPath, user_io_get_file_crc());
        break;
    }

    onion_response_printf(res, "loadfile [%s] [%s]\n",SelectedPath,fs_pFileExt);
    return OCS_PROCESSED;
}

int api_help(void *, onion_request * req, onion_response * res) {
  //onion_response_set_length(res, 11);

  onion_response_write0(res, "<b>API</b><p/>");
  onion_response_write0(res, "/api/screenshot  - no parameters, returns a PNG <br/>");
  onion_response_write0(res, "/api/keypress - key=<javascript keycode>  - sends key to core <br/>");
  onion_response_write0(res, "/api/loadcore- name=menu.rbf - loads the core and restarts MiSTer binary <br/>");
  onion_response_write0(res, "/api/loadfile - name=boot.rom ext=rom - sends the file to the core <br/>");
  onion_response_write0(res, "/api/getconfig - no parameters, returns JSON of core config_str if 8bit<br/>");
  onion_response_write0(res, "/api/filesearch - name=<path>  - returns json with files in the directory <br/>");
  if (onion_request_get_query(req, "1")) {
    onion_response_printf(res, "<p>Path: %s",
                          onion_request_get_query(req, "1"));
  }
  onion_response_printf(res, "<p>Client description: %s",
                        onion_request_get_client_description(req));
  return OCS_PROCESSED;
}


onion *o = NULL;


int web_setup()
{
//  ONION_VERSION_IS_COMPATIBLE_OR_ABORT();
  char base_path[1024];
  char html_path[1024];
  // the rootDir needs to end in a / or it messes up our parsing later
  strcpy(base_path,getRootDir());
  if (base_path[strlen(base_path)-1]!='/')
	  strcat(base_path,"/");

  sprintf(html_path,"%shtml",base_path);
  o = onion_new(O_POOL);
  onion_set_timeout(o, 5000);
  onion_set_hostname(o, "0.0.0.0");
  onion_set_port(o, "80");
  onion_url *urls = onion_root_url(o);

  // Add API URL handlers
  onion_url_add(urls, "^api/screenshot", (void *)screenshot);
  onion_url_add(urls, "^api/keypress", (void *)keypress);
  onion_url_add(urls, "^api/loadcore", (void *)loadcore);
  onion_url_add(urls, "^api/getconfig", (void *)getconfig);
  onion_url_add(urls, "^api/loadfile", (void *)loadfile);
  onion_url_add(urls, "^api/filesearch", (void *)filesearch);
  onion_url_add_with_data(urls, "^api/richfilemanager", (void *) RichFileManager, (void *)base_path , NULL);
  onion_url_add(urls, "^api/(.*)$", (void *)api_help);

  // This places all the static HTML from html directory at / 
  onion_url_add_with_data(urls, "", (void*)onion_shortcut_internal_redirect, (void *)"index.html",NULL);
  onion_handler *dir = onion_handler_export_local_new(html_path);
  onion_handler_add(dir, onion_handler_static("<h1>404 - File not found.</h1>", 404));
  onion_handler_add((onion_handler *)urls,dir);
 

  onion_listen_setup(o);

  return 0;
}

int count=1000;

void web_poll()
{
   // this code is to delay, and do the keyup event if we have a keypress event
             if (code) {
	   count--;
	   if (count<=0) {
		   count=1000;
	 	   user_io_kbd(code, 0);
		   code=0;
	   }
          }

          // poll onion
          onion_listen_poll(o);
}
void web_cleanup()
{
  onion_listen_cleanup(o);

  onion_free(o);
}
