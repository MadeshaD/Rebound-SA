static mixpcm::Format Parse(const WAVEFORMATEX*w){
 mixpcm::Format f;if(!w)return f;
 unsigned tag=w->wFormatTag;
 if(tag==WAVE_FORMAT_EXTENSIBLE){
  if(w->cbSize<22)return f;
  auto*x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
  if(x->Samples.wValidBitsPerSample>w->wBitsPerSample)return f;
  if(IsEqualGUID(x->SubFormat,KSDATAFORMAT_SUBTYPE_PCM))tag=WAVE_FORMAT_PCM;
  else if(IsEqualGUID(x->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))tag=WAVE_FORMAT_IEEE_FLOAT;
  else return f;
 }
 if(tag!=WAVE_FORMAT_PCM&&tag!=WAVE_FORMAT_IEEE_FLOAT)return f;
 f={w->nSamplesPerSec,w->nChannels,w->wBitsPerSample,tag==WAVE_FORMAT_IEEE_FLOAT};
 if(!f.valid()||w->nBlockAlign!=f.channels*f.bytes())return {};
 if(w->wFormatTag==WAVE_FORMAT_EXTENSIBLE){
  const auto* x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
  f.validBits=x->Samples.wValidBitsPerSample;
  if(x->dwChannelMask&SPEAKER_LOW_FREQUENCY)
   f.lfe=__builtin_popcount(x->dwChannelMask&(SPEAKER_LOW_FREQUENCY-1));
 }
 return f;
}
