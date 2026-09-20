// read_subf 提取自用户提供的旧版 PCMProcessDlg.cpp，函数主体原样保留。
// 外围程序独立读取首尾数据，用于检验旧版打开路径，不能代替旧版 GUI 全参数解算。
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
using namespace std;
/** @brief 旧版读帧函数使用的最小配置结构。 */
struct Headers {
    int frlength[1], idnum[1], id_len[1], frdepth[1], id_sbit[1], time_mode[1];
};
/** @brief 保留旧版固定缓冲与读帧函数，并在缓冲尾部检查越界哨兵。 */
class LegacyOpenProbe {
public:
    Headers heads{};
    unsigned short subcod[65535]{};
    unsigned short frame[2048]{};
    uint64_t canary = 0x123456789abcdef0ULL;
    long long dwBlocki=0, dwBlockBytes=0;
    int nwfr=0, idadd=0, idv=0, DatErro=0, OneSubFrameID=0;
    unsigned int maskw=0;
    char *MemoryData=nullptr;
    int read_subf(int strea,int itlAddr);
};
int LegacyOpenProbe::read_subf(int strea,int itlAddr)  // read one whole longframe into subcod[16384];

{

	int i;

	nwfr=heads.frlength[strea];    //一个短周的短周字数

	idadd=heads.idnum[strea];

	maskw=0;

	for(i=0;i<heads.id_len[strea];i++)

		maskw=maskw+(short)pow((float)2,i);

Loop2:



	itlAddr=dwBlocki;

	if(dwBlocki+2*heads.frdepth[strea]*heads.frlength[strea]>=dwBlockBytes)

	{

		

		return 2;

	}



	memcpy(frame,&(MemoryData[itlAddr]),2*nwfr);

	if((frame[0]!= 65131 && frame[1]!= 10304)   && (frame[0]!= 60304))

	{

		dwBlocki++;

		goto Loop2;

	}



	do

	{

		memcpy(frame,&(MemoryData[itlAddr]),nwfr*2);

		if((frame[0]!= 65131 && frame[1]!= 10304)  && (frame[0]!= 60304))

		{

			return 3;

		}

		if(DatErro>heads.frdepth[strea]*10)

		{

			DatErro=0;

		}

		if(heads.time_mode[strea]==6)

			idv=((frame[idadd]&0xFF)>>heads.id_sbit[strea])&maskw;

		else

			idv=(frame[idadd]>>heads.id_sbit[strea])&maskw;

		if((idv-OneSubFrameID)!=0)

		{

			OneSubFrameID=0;

			DatErro=DatErro+1;

			itlAddr = itlAddr +  2*nwfr; 

			dwBlocki = itlAddr;

			continue;

		}

		for(i=0;i<nwfr;i++)

			subcod[nwfr*OneSubFrameID+i]=(frame[i]&0xffff); //一个短周

		// subcod[nwfr*OneSubFrameID+i]=subcod[frlength*frame_no+word_no]

		OneSubFrameID++;



		itlAddr = itlAddr +  2*nwfr; 

		dwBlocki = itlAddr;



	}while((OneSubFrameID<heads.frdepth[strea])&&(itlAddr+2*heads.frdepth[strea]*heads.frlength[strea] < dwBlockBytes));



	if(OneSubFrameID = heads.frdepth[strea])

		OneSubFrameID=0;

	if(itlAddr+2*heads.frdepth[strea]*heads.frlength[strea] >= dwBlockBytes)

		return 2;

	return 1;

}



/** @brief 对首尾数据执行旧版打开路径的读帧调用，并检查完整长周和越界哨兵。 */
int main(int argc,char **argv) {
    if(argc!=5) return 2;
    const int words=stoi(argv[2]), depth=stoi(argv[3]), id=stoi(argv[4]);
    if(words<=0 || words>2048 || depth<=0 || words*depth>32767 || id<0 || id>=words) return 3;
    ifstream input(argv[1],ios::binary|ios::ate);
    if(!input) return 4;
    long long size=input.tellg(), major=words*depth*2LL;
    if(size<major*8 || size%major!=0) return 5;
    LegacyOpenProbe probe;
    probe.heads.frlength[0]=words; probe.heads.frdepth[0]=depth;
    probe.heads.idnum[0]=id; probe.heads.id_len[0]=16;
    long long offsets[2]={0,size-5*major};
    for(int sample=0;sample<2;sample++) {
        long long amount=min(314572800LL,size-offsets[sample]);
        vector<char> bytes(static_cast<size_t>(amount));
        input.seekg(offsets[sample]); input.read(bytes.data(),amount);
        if(input.gcount()!=amount) return 6;
        probe.MemoryData=bytes.data(); probe.dwBlockBytes=amount;
        probe.dwBlocki=0; probe.OneSubFrameID=0;
        int first=probe.read_subf(0,0);
        if(first!=1) return 7;
        // The old start-time branch calls read_subf a second time.
        if(sample==0 && probe.read_subf(0,static_cast<int>(probe.dwBlocki))!=1) return 8;
        if(probe.canary!=0x123456789abcdef0ULL) return 9;
        for(int subframe=0;subframe<depth;subframe++) {
            const int position=subframe*words;
            if(probe.subcod[position]!=0xfe6b || probe.subcod[position+1]!=0x2840 ||
               probe.subcod[position+id]!=subframe) return 10;
        }
        cout << (sample==0 ? "begin":"tail") << " offset=" << offsets[sample]
             << " cursor=" << probe.dwBlocki << " time_words="
             << probe.subcod[3] << "," << probe.subcod[4] << ","
             << probe.subcod[5] << "," << probe.subcod[6] << "\n";
    }
    cout << "legacy_read_subf_passed size=" << size << " frame_words=" << words
         << " depth=" << depth << " canary=unchanged\n";
}
