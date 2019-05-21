#include <signal.h>
#include <string.h>
#include <stdio.h>

#include <onion/onion.h>
#include <onion/log.h>
#include <onion/dict.h>
#include <onion/shortcuts.h>
#include <onion/block.h>

#include <dirent.h>
#include <sys/stat.h>
#include <onion/shortcuts.h>
#include <locale.h>
#include <libintl.h>
#include <onion/handlers/webdav.h>
#include <onion/handlers/exportlocal.h>



void print_file_json(onion_response * res,const char *base, const char *full)
{
	int addslash=0;
	// pull apart path, name, and full
	//
	//full:[/data1/roms/MiSTer_norbf/a.centiped/Centipede (revision 2).rom]
	//base:[/data1/roms/MiSTer_norbf]
	//path:[/a.centiped]
	//name:[Centipede (revision 2).rom]

        // name is from after the / to the end
	char name[1024];
	name[0]=0;
	char path[1024];
	path[0]=0;
	char *p = strrchr(full,'/');
	if (p)
		strcpy(name,p+1);
	// path is from the base to the / 
	strcpy(path,full+strlen(base)-1);
	p = strrchr(path,'/');
	if (p) *p=0;

	#if 1
	printf("full:[%s]\n",full);
	printf("base:[%s]\n",base);
	printf("path:[%s]\n",path);
	printf("name:[%s]\n",name);
	#endif

        struct stat st;
        stat(full, &st);

        if (S_ISDIR(st.st_mode))
	{
		if (name[strlen(name)-1]!='/')
			addslash=1;
	}

        onion_response_printf(res, " {  ");
        if (!strcasecmp("/",path))
		onion_response_printf(res, " \"id\": \"/%s%s\",   ",name,addslash?"/":"");
	else
        	onion_response_printf(res, " \"id\": \"%s/%s%s\",   ",path,name,addslash?"/":"");
        if (S_ISDIR(st.st_mode))
          onion_response_printf(res, " \"type\": \"folder\",   ");
        else
          onion_response_printf(res, " \"type\": \"file\",   ");
        onion_response_printf(res, " \"attributes\":  {  ");
	

        onion_response_printf(res, " \"name\": \"%s\",   ",name);
        onion_response_printf(res, " \"path\": \"%s%s\",   ",full,addslash?"/":"");
        onion_response_printf(res, " \"readable\": \"%d\",   ",((st.st_mode & S_IRUSR) == S_IRUSR));
        onion_response_printf(res, " \"writeable\": \"%d\",   ",(st.st_mode & S_IWUSR) == S_IWUSR);
        onion_response_printf(res, " \"created\": \"%ld\",   ",st.st_ctim.tv_sec);
        onion_response_printf(res, " \"modified\": \"%ld\",   ",st.st_mtim.tv_sec);
        onion_response_printf(res, " \"size\": \"%lld\"   ",st.st_size);
        onion_response_printf(res, " } ");
        onion_response_printf(res, " } ");
}

void print_dir_json(onion_response * res, const char *basepath,const char *path)
{
    bool first=true;
    char dirname[256];
    snprintf(dirname, sizeof(dirname), "%s%s", basepath,path);
    char *realp = realpath(dirname, NULL);
    if (!realp) {
        ONION_WARNING("print_dir_json realpath returned null dirname:[%s] basepath[%s] path[%s]\n",dirname,basepath,path);
	
      return;
    }

    DIR *dir = opendir(realp);
    if (dir) {                  // its a dir, fill the dictionary.

    onion_response_printf(res, " { \"data\": [    ");

      struct dirent *de;
      while ((de = readdir(dir))) {     // Fill one files.[filename] per file.
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s/%s", realp, de->d_name);
        struct stat st;
        stat(tmp, &st);


       if (!strcasecmp(".",de->d_name))
	       continue;
       if (!strcasecmp("..",de->d_name))
	       continue;

	if (first==false)
             onion_response_printf(res, " ,  ");
	else
		first=false;

#if 1
	print_file_json(res,basepath,tmp);
#else
        onion_response_printf(res, " {  ");
        if (!strcasecmp("/",path))
		onion_response_printf(res, " \"id\": \"/%s\",   ",de->d_name);
	else
        	onion_response_printf(res, " \"id\": \"%s/%s\",   ",path,de->d_name);
        if (S_ISDIR(st.st_mode))
          onion_response_printf(res, " \"type\": \"folder\",   ");
        else
          onion_response_printf(res, " \"type\": \"file\",   ");
        onion_response_printf(res, " \"attributes\":  {  ");
	


        onion_response_printf(res, " \"name\": \"%s\",   ",de->d_name);
        onion_response_printf(res, " \"path\": \"%s\",   ",tmp);
        onion_response_printf(res, " \"readable\": \"%d\",   ",((st.st_mode & S_IRUSR) == S_IRUSR));
        onion_response_printf(res, " \"writeable\": \"%d\",   ",(st.st_mode & S_IWUSR) == S_IWUSR);
        onion_response_printf(res, " \"created\": \"%ld\",   ",st.st_ctim.tv_sec);
        onion_response_printf(res, " \"modified\": \"%ld\",   ",st.st_mtim.tv_sec);
        onion_response_printf(res, " \"size\": \"%ld\"   ",st.st_size);
        onion_response_printf(res, " } ");
        onion_response_printf(res, " } ");
#endif

      }
      closedir(dir);
      free(realp);
    onion_response_printf(res, "   ]}");

      return ;
    }

}


void print_recursive_search_json(onion_response * res, const char *basepath, const char *name, const char *search, int *first)
{
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(name)))
        return ;

    //printf("name [%s] search [%s] first %d\n",name,search,*first);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            char path[1024];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", name, entry->d_name);
            if (!strncasecmp(entry->d_name,search,strlen(search)))
	    {
		if (*first) 
		{
			*first = 0;
		}
		else {
    			onion_response_printf(res, ",");
		}
		print_file_json(res,basepath,path);
	    }
            //listdir(search,path, indent + 2);
	    print_recursive_search_json(res, basepath, path,search,first);
        } else {
            if (!strncasecmp(entry->d_name,search,strlen(search))) {
		if (*first) 
		{
			*first = 0;
		}
		else {
    			onion_response_printf(res, ",");

		}
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", name, entry->d_name);
		print_file_json(res,basepath,path);
	    }
        }
    }
    closedir(dir);
}



// check to see if this is trying to get out of our root
int is_safe_path(const char *base, const char *path) {
    char dirname[PATH_MAX];
    char realp[PATH_MAX];
    char newpath[PATH_MAX];
    // we can't compare if we end up with a path with // in it
    if (path[0]=='/')
	    strcpy(newpath,path+1);
    else
	    strcpy(newpath,path);

    
    snprintf(dirname, sizeof(dirname), "%s%s", base,newpath);
    printf("dirname[%s]\n",dirname);
    if (dirname[strlen(dirname)-1]=='/')
     dirname[strlen(dirname)-1]=0;
    printf("dirname[%s]\n",dirname);

    realpath(dirname, realp);
    printf("realp[%s]\n",realp);
    if (strncmp(realp, dirname,strlen(dirname)) != 0) {        // out of secured dir.
        ONION_WARNING("is_safe_path: NO base[%s] path [%s]",base,path);
    	return 0;
    }
    return 1;
}

void error(onion_response *res,const char * title,const char *arguments)
{
        onion_response_printf(res, " { ");
        onion_response_printf(res, " \"errors\": [ ");
        onion_response_printf(res, "  { ");
        onion_response_printf(res, "  \"id\": \"server\", ");
        onion_response_printf(res, "  \"code\": \"500\", ");
        onion_response_printf(res, "  \"title\": \"%s\" ",title);
	if (arguments) {
        	onion_response_printf(res, " ,");
        	onion_response_printf(res, "  \"meta\": { ");
        	onion_response_printf(res, "    \"arguments\": [ \"%s\"]  ",arguments);
        	onion_response_printf(res, "  } ");
	}
       	onion_response_printf(res, " } ");
       	onion_response_printf(res, " ] ");
       	onion_response_printf(res, " } ");
}

// needs ending slash to be safe
//#define BASEPATH "/data1/roms/MiSTer_norbf/"

int RichFileManager(void *p, onion_request * req, onion_response * res) {
	const char *BASEPATH=(const char *)p;
	char basepath[1024];
	if (BASEPATH==NULL) {
		printf("BASEPATH is NULL\n");
		strcpy(basepath,"/media/fat/");
		BASEPATH=basepath;	
	}

    if (onion_request_get_flags(req) & OR_POST) 
    {
        const char *mode=onion_request_get_post(req,"mode");
        ONION_WARNING("filemanager: mode: [%s]",mode);

        if (!strcasecmp(mode,"savefile")) {
            const char *path=onion_request_get_post(req,"path");
            const char *content=onion_request_get_post(req,"content");
            if (is_safe_path(BASEPATH,path)) {
               char fullname[1024];
               snprintf(fullname, sizeof(fullname), "%s%s", BASEPATH,path);
	       FILE *f=fopen(fullname,"w");
	       fwrite(content,strlen(content),1,f);
	       fclose(f);
               onion_response_set_header(res, "Content-Type", "text/json");
    	       onion_response_printf(res, " { \"data\":     ");
	       print_file_json(res, BASEPATH,fullname);
    	       onion_response_printf(res, " }");
	    } else {
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unsafe pathname or filename",path);
	    }

	} else if (!strcasecmp(mode,"upload")) {
            const char *path=onion_request_get_post(req,"path");
            const char *name = onion_request_get_post(req, "files");
            const char *filename = onion_request_get_file(req, "files");
            if (name && filename && is_safe_path(BASEPATH,path)) {
               char fullname[1024];
	       const char *ptr=path;
	       if (*ptr=='/') ptr++;
               snprintf(fullname, sizeof(fullname), "%s%s%s", BASEPATH,ptr,name);
	    printf("path[%s]\n",ptr);
	    printf("name[%s]\n",name);
	    printf("filename[%s]\n",filename);
	    printf("fullname[%s]\n",fullname);
      	    ONION_DEBUG("Copying from %s to %s", filename, fullname);
      	    // this rename seems to work from /tmp
      	    // do we want the tmp to be /tmp?? need to think about that, look..
      	    onion_shortcut_rename(filename, fullname);
            onion_response_set_header(res, "Content-Type", "text/json");
            onion_response_printf(res, " { \"data\": [    ");
            print_file_json(res, BASEPATH,fullname);
            onion_response_printf(res, "] }");
	    }
	} else{
		ONION_ERROR("unimplemented mode POST: [%s]\n",mode);
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unimplemented",mode);
	}
    }
    else {
    const char *mode=onion_request_get_queryd(req,"mode","");
    ONION_WARNING("filemanager: mode: [%s]",mode);


  

  if (!strcasecmp(mode,"initiate")) {
    onion_response_set_header(res, "Content-Type", "text/json");
    onion_response_printf(res, " { \"data\": { \"id\": \"/\",      ");
    onion_response_printf(res, "     \"type\": \"initiate\",       ");
    onion_response_printf(res, "     \"attributes\": {             ");
    onion_response_printf(res, "      \"config\": {             ");
    onion_response_printf(res, "       \"upload\": {             ");
    onion_response_printf(res, "         \"fileSizeLimit\": 16000000              ");
    onion_response_printf(res, "            },                                   ");
    onion_response_printf(res, "       \"security\": {             ");
    onion_response_printf(res, "         \"readOnly\": false,              ");
    onion_response_printf(res, "         \"allowFolderDownload\": false,              ");
    onion_response_printf(res, "         \"extensions\": {             ");
    onion_response_printf(res, "             \"ignoreCase\": true,              ");
    onion_response_printf(res, "             \"policy\": \"DISALLOW_LIST\",              ");
    onion_response_printf(res, "             \"restrictions\": []              ");
    onion_response_printf(res, "            },                                   ");
    onion_response_printf(res, "       \"options\": {             ");
    onion_response_printf(res, "         \"culture\": \"en\"              ");
    onion_response_printf(res, "            }                                   ");
    onion_response_printf(res, "            }                                   ");
    onion_response_printf(res, "            }                                   ");
    onion_response_printf(res, "            }                                   ");
    onion_response_printf(res, "            }                                   ");
    onion_response_printf(res, "            }                                   ");
  } else if (!strcasecmp(mode,"readfolder")) {
    onion_response_set_header(res, "Content-Type", "text/json");
    const char *path=onion_request_get_queryd(req,"path","");
    print_dir_json(res,BASEPATH ,path);
  } else if (!strcasecmp(mode,"readfile") || !strcasecmp(mode,"getimage")) {
    const char *path=onion_request_get_queryd(req,"path","");
    if (is_safe_path(BASEPATH,path)) {
       char fullname[1024];
       snprintf(fullname, sizeof(fullname), "%s%s", BASEPATH,path);
     return onion_shortcut_response_file(fullname, req, res);
    }
    else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unsafe pathname or filename",path);
    }

  }
  else if (!strcasecmp(mode,"getinfo")) {
    const char *path=onion_request_get_queryd(req,"path","");
    if (is_safe_path(BASEPATH,path)) {
        char fullname[1024];
        snprintf(fullname, sizeof(fullname), "%s%s", BASEPATH,path);
        onion_response_set_header(res, "Content-Type", "text/json");
        onion_response_printf(res, " { \"data\":     ");
        print_file_json(res, BASEPATH,fullname);
        onion_response_printf(res, " }");
       } else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unsafe pathname or filename",path);
       }
  } else if (!strcasecmp(mode,"seekfolder")) {
    const char *path=onion_request_get_queryd(req,"path","");
    const char *string=onion_request_get_queryd(req,"string","");
    if (is_safe_path(BASEPATH,path)) {
       char foldername[1024];
        snprintf(foldername, sizeof(foldername), "%s%s", BASEPATH,path);
    		onion_response_set_header(res, "Content-Type", "text/json");
    	       onion_response_printf(res, " { \"data\":     ");
    	       onion_response_printf(res, " [      ");
	       int first=1;
	       print_recursive_search_json(res,BASEPATH,foldername,string,&first);
    	       onion_response_printf(res, " ]      ");
    	       onion_response_printf(res, " }");
       } else {
               onion_response_set_header(res, "Content-Type", "text/json");
    	       onion_response_printf(res, " { \"data\":     ");
	       print_file_json(res, BASEPATH,path);
    	       onion_response_printf(res, " }");
       }
  } else if (!strcasecmp(mode,"addfolder")) {
    const char *path=onion_request_get_queryd(req,"path","");
    const char *name=onion_request_get_queryd(req,"name","");
    char foldername[1024];
    sprintf(foldername,"%s/%s",path,name);
    if (is_safe_path(BASEPATH,foldername)) {
       char fullname[1024];
       snprintf(fullname, sizeof(fullname), "%s%s", BASEPATH,foldername);
       int ret = mkdir(fullname,0777);
       if (ret!=0){ 
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"addfolder failed",strerror(ret));
       } else {
               onion_response_set_header(res, "Content-Type", "text/json");
    	       onion_response_printf(res, " { \"data\":     ");
	       print_file_json(res, BASEPATH,fullname);
    	       onion_response_printf(res, " }");
       }

    }
    else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unsafe pathname or filename",foldername);
    }

  } else if (!strcasecmp(mode,"download")) {
            const char *path=onion_request_get_queryd(req,"path","");
	       printf("path[%s]\n",path);
	         const onion_dict *d=onion_request_get_header_dict( req);
		   onion_dict_print_dot(d);
// NOTE - there is supposed to be an AJAX download we are supposed to look for, not sure I am seeing the web request
            if (is_safe_path(BASEPATH,path)) {
               char fullname[1024];
               snprintf(fullname, sizeof(fullname), "%s%s", BASEPATH,path);
	       printf("path[%s]\n",path);
	       char *name;
	       char namep[1024];
	       strcpy(namep,path);
	       if (namep[strlen(namep)-1]=='/') {
	       	namep[strlen(namep)-1]=0;
	       }
	       name = strrchr(namep,'/');
               char content_str[1024];
	       sprintf(content_str,"attachment; filename=\"%s\"",name+1);
	       printf("name:%s",name);
	       printf("fullname:%s",fullname);
    		onion_response_set_header(res, "Content-Disposition", content_str);
		return onion_shortcut_response_file(fullname,req,res);

	    } else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unsafe pathname or filename",path?path:"");
	    }

  } else if (!strcasecmp(mode,"delete")) {
            const char *path=onion_request_get_queryd(req,"path","");
            if (is_safe_path(BASEPATH,path)) {
               char fullname[1024];
               snprintf(fullname, sizeof(fullname), "%s%s", BASEPATH,path);
	       printf("path[%s]\n",path);
	       printf("remove: [%s]\n",fullname);
               onion_response_set_header(res, "Content-Type", "text/json");
    	       onion_response_printf(res, " { \"data\":     ");
	       print_file_json(res, BASEPATH,fullname);
    	       onion_response_printf(res, " }");
	       remove(fullname);
	    } else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unsafe pathname or filename",path?path:"");
	    }
  } else if (!strcasecmp(mode,"rename") || !strcasecmp(mode,"move")) {
      // we need to check old and new path -- we need a function for that!
      const char *old=onion_request_get_queryd(req,"old","");
      const char *new=onion_request_get_queryd(req,"new","");
      printf("about to rename: [%s] [%s]\n",old,new);
      if (is_safe_path(BASEPATH,old) && is_safe_path(BASEPATH,new)) {
       char oldname[1024];
       snprintf(oldname, sizeof(oldname), "%s%s", BASEPATH,old);
       char newname[1024];
       snprintf(newname, sizeof(newname), "%s%s", BASEPATH,new);
       if ( !strcasecmp(mode,"move")) {
	       char oldp[1024];
	       strcpy(oldp,old);
	       char *name;
	       if (oldp[strlen(oldp)-1]=='/') {
	       	oldp[strlen(oldp)-1]=0;
	       }
	       name = strrchr(oldp,'/');
	       const char *newp = new;
	       if (*newp=='/') newp++;
	       if (!name) {
		       printf("name is null\n");
	       }
	       else{
              snprintf(newname, sizeof(newname), "%s%s%s", BASEPATH,newp,name+1);
              printf("newname: [%s]\n",newname);
	       }

       }
       printf("about to rename: [%s] [%s]\n",old,new);
       printf("about to rename: [%s] [%s]\n",oldname,newname);
       int ret = rename(oldname, newname);
       if (ret!=0){ 
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"rename failed",strerror(ret));
       } else {
               onion_response_set_header(res, "Content-Type", "text/json");
    	       onion_response_printf(res, " { \"data\":     ");
	       print_file_json(res, BASEPATH,newname);
    	       onion_response_printf(res, " }");
       }
      }
      else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		char error_str[1024];
		sprintf(error_str,"[%s] [%s]",new,old);
		error(res,"unsafe pathname or filename",error_str);
      }

  }
 else{
    		onion_response_set_header(res, "Content-Type", "text/json");
		onion_response_set_code(res, 500);
		error(res,"unimplemented",mode);
		ONION_ERROR("unimplemented mode GET: [%s]\n",mode);
 }
 }

  return OCS_PROCESSED;

}
