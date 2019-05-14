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

#include <onion/onion.h>

#ifdef __cplusplus
extern "C" {
#endif

int screenshot(void *p, onion_request * req, onion_response * res);
#ifdef __cplusplus
}
#endif


int getScreenshotBuf(unsigned char **buf,unsigned int *outsize)
{
		        printf("print key pressed - do screen shot\n");
                        mister_scaler *ms=mister_scaler_init();
                        if (ms==NULL)
                        {
                                printf("problem with scaler, maybe not a new enough version\n");
				return -1;
                        }
                        else
                        {
                        unsigned char *outputbuf = (unsigned char *)calloc(ms->width*ms->height*3,1);
                        mister_scaler_read(ms,outputbuf);
                        //user_io_get_core_name()
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

int screenshot(void *p, onion_request * req, onion_response * res) {
  unsigned char *buf;
  unsigned int outsize;
  onion_response_set_header(res, "Content-Type", "image/png");
  if (onion_response_write_headers(res) == OR_SKIP_CONTENT)     // Maybe it was HEAD.
    return OCS_PROCESSED;

  int result = getScreenshotBuf(&buf,&outsize);
  ssize_t size= onion_response_write(res, (const char*)buf,outsize);
  free(buf);
  fprintf(stdout,"screenshot: result: %d size: %d\n",result,size);
  return OCS_PROCESSED;

}

int loadcore(void *p, onion_request * req, onion_response * res) {
	fprintf(stderr,"loadcore\n");
  const char *core=onion_request_get_queryd(req,"name","menu.rbf");
  onion_response_write0(res, "done");
  onion_response_printf(res, "<p>name: %s",core);
  fprintf(stderr,"loadcore:%s\n",core);
  int result=fpga_load_rbf(core);
  fprintf(stderr,"loadcore:%s returning\n",core);
  return OCS_PROCESSED;

}

int getconfig(void *pp, onion_request * req, onion_response * res) {
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

int loadfile(void *p, onion_request * req, onion_response * res) {
	int id = fpga_core_id();
  	const char *SelectedPath=onion_request_get_queryd(req,"name","boot.rom");
  	const char *fs_pFileExt=onion_request_get_queryd(req,"ext","rom");
	switch(id)
	{
		case CORE_TYPE_8BIT:
                printf("File selected: %s\n", SelectedPath);
                //user_io_file_tx(SelectedPath, user_io_ext_idx(SelectedPath, fs_pFileExt) << 6 | (menusub + 1), opensave);
                user_io_file_tx(SelectedPath, user_io_ext_idx((char *)SelectedPath, (char *)fs_pFileExt) << 6 | (0+ 1) );
                cheats_init((char *)SelectedPath);
                
                break;
	}
  onion_response_printf(res, "loadfile [%s] [%s]\n",SelectedPath,fs_pFileExt);
}

int hello(void *p, onion_request * req, onion_response * res) {
  //onion_response_set_length(res, 11);

  onion_response_write0(res, "Hello world");
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


  o = onion_new(O_POOL);
  onion_set_timeout(o, 5000);
  onion_set_hostname(o, "0.0.0.0");
  onion_url *urls = onion_root_url(o);

  onion_url_add_static(urls, "static", "Hello static world", HTTP_OK);
  onion_url_add(urls, "screenshot", (void *)screenshot);
  onion_url_add(urls, "loadcore", (void *)loadcore);
  onion_url_add(urls, "getconfig", (void *)getconfig);
  onion_url_add(urls, "loadfile", (void *)loadfile);
  onion_url_add(urls, "", (void *)hello);
  onion_url_add(urls, "^(.*)$", (void *)hello);

  onion_listen_setup(o);

  return 0;
}
void web_poll()
{
   onion_listen_poll(o);

}
void web_cleanup()
{
  onion_listen_cleanup(o);

  onion_free(o);
}
