/**
    Licensed under GNU/GPLv3
    Author: DaVieS at nPulse.net / Viktor Hlavaji
    Mail: davies@npulse.net
    Software is intended to recover files from a bad drive, written in pure C.
    Compile with: gcc -o pfr pfr.c | clang -o pfr pfr.c
**/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> 

#define E_OK 0
#define E_FAIL 1
#define VERSION "1.0r"
#define true 1
#define false 0

typedef char bool;
typedef long int _long;
typedef struct
{
    char buffer[65540];
    int slen;
} storage;



int main(int argc, char *argv[])
{
    unsigned int i;
    FILE * src;
    FILE * dst;
    _long src_size = 0;
    _long cur_pointer = 0;
    _long retries = 1;
    char fill[3];
    char tmp[65540];
    char _fill[2];
    _long badBlocks = 0;
    _long rbadBlocks = 0;
    _long retry = 0;
    storage buff;
    size_t slen,plen;
    bool success;
    time_t st = 0;
    float progress;
    bool badBlock;
    bool readAhead;
    int clean;

    fill[0] = 0x00;
    strcat(fill,"00");

    buff.buffer[0] = 0x00;
    buff.slen = 0;

    fprintf(stderr,"\n\n[P]artial [F]ile [R]ecovery Version: %s\n",VERSION);
    fprintf(stderr,"Author: Viktor Hlavaji aka DaVieS, davies@npulse.net | Licensed under: GNU/GPLv3\n\n");

    if(argc < 3)
    {
        fprintf(stderr,"Usage: %s [source_file] [dest_file] [try = 1] [fill = 00]\n\n",argv[0]);
        fprintf(stderr," - source_file: The file that needs recover\n");
        fprintf(stderr," - dest_file: Output file will generated by this software\n");
        fprintf(stderr," - try: (Optional) number of attempts to read a badSector\n");
        fprintf(stderr," - fill: (Optional) unrecovered bytes will replaced by hexCode 0x[XX]\n\n");

        fprintf(stderr,"Software Output:\n");
        fprintf(stderr,"Action | Current Block: 315385359, Round Left: 1, lost: 26127, recovered: 12, acquired: 315359232, Progress: 60.154984\n");
        fprintf(stderr," |       |                         |              |            |              |                    |-> Progress\n");
        fprintf(stderr," |       |                         |              |            |              |-> Bytes read successfully\n");
        fprintf(stderr," |       |                         |              |            |-> Bytes magically recovered\n");
        fprintf(stderr," |       |                         |              |-> Unrecovered Bytes that filled with a specified character\n");
        fprintf(stderr," |       |                         |-> Retry Count Left\n");
        fprintf(stderr," |       |-> Show current position\n");
        fprintf(stderr," |-> [Fast Forward] when continous data read available, or [Block Mining] when recovery in action\n\n");

        fprintf(stderr,"Example: %s ./broken_file.mp3 ./broken_file_fix.mp3\n",argv[0]);
        fprintf(stderr,"Example: %s /home/music/broken_file.mp3 /tmp/broken_file_fix.mp3 2 00\n\n",argv[0]);
        return E_FAIL;
    }


    if(argc >= 5)
    {
        if(strlen(argv[4]) != 2)
        {
            fprintf(stderr,"Invalid Argument\n\n");
            return E_FAIL;
        }

        fill[0] = 0x00;

        for(i=0;i<=255;i++)
        {
            tmp[0] = 0x00;
            sprintf(tmp,"%02X",i);
            if(!strcmp(argv[4],tmp))
            {
                strcat(fill,tmp);
                _fill[0] = (char)i;
                _fill[1] = 0x00;
            }
        }

        if(fill[0] == 0x00)
        {
            fprintf(stderr,"Invalid Argument\n\n");
            return E_FAIL;
        }

    }

    if(argc >= 4)
    {
        retries = atoi(argv[3]);
    }


    src = fopen (argv[1],"r");
    if (src == NULL)
    {
        fprintf(stderr,"Unable to open source file for reading: %s\n\n",argv[1]);
        return E_FAIL;
    }    
    else
    {
        if(fseek(src,0,SEEK_END) != 0)
        {
            fprintf(stderr,"File is unrecoverable, seek failed: %s\n\n",argv[1]);
            fclose (src);
            return E_FAIL;
        }
        src_size = ftell(src); 
        if(src_size <= 0)       
        {
            fprintf(stderr,"File is unrecoverable, seek failed: %s\n\n",argv[1]);
            fclose (src);
            return E_FAIL;
        }
    }

    dst = fopen (argv[2],"w");
    if (dst == NULL)
    {
        fprintf(stderr,"Unable to open destination file for writing: %s\n\n",argv[2]);
        fclose (src);
        return E_FAIL;
    }    


    fprintf(stdout,"\n");
    fprintf(stdout,"Source File Opened Successfully: %s\n",argv[1]);
    fprintf(stdout,"Destination File Opened Successfully: %s\n",argv[2]);
    fprintf(stdout,"Total Bytes to recover: %ld\n",src_size);
    fprintf(stdout,"badBlock Try Count: %li\n",retries);
    fprintf(stdout,"badBlock fill char: 0x%s\n\n",fill);

    cur_pointer = 0;
    fseek(src,0,SEEK_SET);
    readAhead = true;

    while(true)
    {
        success = false;
        badBlock = false;
        retry = retries;

        if(!readAhead && clean >= 65536)
        {
            readAhead = true;
        }

        if(readAhead && (cur_pointer + 65536) < src_size)
        {
            if(st != time(NULL))
            {
                progress = (float)((double)(double)100/(double)src_size)*(double)cur_pointer;
                fprintf(stderr,"Fast Forward | Current Block: %ld, Round Left: %li, lost: %ld, recovered: %ld, acquired: %ld, Progress: %f\n",cur_pointer,retry,badBlocks,rbadBlocks,(cur_pointer - badBlocks),progress);
                st = time(NULL);
            }

            tmp[0] = 0x00;
            slen = fread (tmp, 1, 65536, src);
            if(slen == 65536)
            {
                fwrite(tmp,1,65536,dst);
                cur_pointer = ftell(src);
                continue;
            }            
            else
            {
                fseek(src,cur_pointer,SEEK_SET);
                readAhead = false;
                clean = 0;
            }
        }

        while(retry--)
        {
            if(st != time(NULL))
            {
                progress = (float)((double)(double)100/(double)src_size)*(double)cur_pointer;
                fprintf(stderr,"Block Mining | Current Block: %ld, Round Left: %li, lost: %ld, recovered: %ld, acquired: %ld, Progress: %f\n",cur_pointer,retry,badBlocks,rbadBlocks,(cur_pointer - badBlocks),progress);
                st = time(NULL);
            }

            if(feof(src))
            {
                badBlock = true;
                fclose(src);
                src = fopen (argv[1],"r");
                if (src == NULL)
                {
                    fprintf(stderr,"Unable to open source file for reading: %s\n\n",argv[1]);
                    return E_FAIL;
                }    
            }

            tmp[0] = 0x00;
            slen = fread (tmp, 1, 1, src);
            if(slen == 1)
            {
                buff.buffer[buff.slen] = tmp[0];
                buff.slen++;

                if(buff.slen >= 65536) // 64K CACHING !!
                {
                    fwrite(buff.buffer,buff.slen,1,dst); 
                    buff.slen = 0;
                    buff.buffer[0] = 0x00;
                }

                success = true;
                clean++;
                break;
            }
            else
            {
                badBlock = true;
                continue;
            }
        }

        if(!success)
        {
            clean = 0;
            badBlocks++;
            buff.buffer[buff.slen] = _fill[0];
            buff.slen++;

            if(buff.slen >= 65536) // 64K CACHING !!
            {
                fwrite(buff.buffer,1,buff.slen,dst); 
                buff.slen = 0;
                buff.buffer[0] = 0x00;
            }
        }
        else if(badBlock)
        {
            rbadBlocks++;
        }

        cur_pointer++;
        if(cur_pointer >= src_size)
        {
            break;
        }
        fseek(src,cur_pointer,SEEK_SET);
    }

    if(buff.slen >= 0)
    {
        fwrite(buff.buffer,1,buff.slen,dst); 
        buff.slen = 0;
        buff.buffer[0] = 0x00;
    }


    fclose(src);
    fclose(dst);

    fprintf(stderr,"Finished | Current Block: %ld, Round Left: %li, lost: %ld, recovered: %ld, acquired: %ld, Progress: 100\n",cur_pointer,retry,badBlocks,rbadBlocks,(cur_pointer - badBlocks));


    return E_OK;
}