 /*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AESinkAUDIOTRACK.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include "cores/AudioEngine/Utils/AEPackIEC61937.h"
#include "android/activity/XBMCApp.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#if defined(HAS_LIBAMCODEC)
#include "utils/AMLUtils.h"
#endif
#include "utils/log.h"
#include "utils/StringUtils.h"

#include "android/jni/AudioFormat.h"
#include "android/jni/AudioManager.h"
#include "android/jni/AudioTrack.h"
#include "android/jni/Build.h"
#include "android/jni/System.h"

#include <algorithm>
#include <iostream>
#include <sstream>

using namespace jni;

/*
 * ADT-1 on L preview as of 2014-10 downmixes all non-5.1/7.1 content
 * to stereo, so use 7.1 or 5.1 for all multichannel content for now to
 * avoid that (except passthrough).
 * If other devices surface that support other multichannel layouts,
 * this should be disabled or adapted accordingly.
 */
#define LIMIT_TO_STEREO_AND_5POINT1_AND_7POINT1 1

#define CONSTANT_BUFFER_SIZE_SD 16384
#define CONSTANT_BUFFER_SIZE_HD 32768

#define TRUEHD_UNIT 960
#define SMOOTHED_DELAY_MAX 10
#define SKIP_DELAY_MAX 10

static const AEChannel KnownChannels[] = { AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_LFE, AE_CH_SL, AE_CH_SR, AE_CH_BL, AE_CH_BR, AE_CH_BC, AE_CH_BLOC, AE_CH_BROC, AE_CH_NULL };

static AEChannel AUDIOTRACKChannelToAEChannel(int atChannel)
{
  AEChannel aeChannel;

  /* cannot use switch since CJNIAudioFormat is populated at runtime */

       if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT)            aeChannel = AE_CH_FL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT)           aeChannel = AE_CH_FR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER)          aeChannel = AE_CH_FC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY)         aeChannel = AE_CH_LFE;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT)             aeChannel = AE_CH_BL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT)            aeChannel = AE_CH_BR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT)             aeChannel = AE_CH_SL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT)            aeChannel = AE_CH_SR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER)  aeChannel = AE_CH_FLOC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER) aeChannel = AE_CH_FROC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER)           aeChannel = AE_CH_BC;
  else                                                                      aeChannel = AE_CH_UNKNOWN1;

  return aeChannel;
}

static int AEChannelToAUDIOTRACKChannel(AEChannel aeChannel)
{
  int atChannel;
  switch (aeChannel)
  {
    case AE_CH_FL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT; break;
    case AE_CH_FR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT; break;
    case AE_CH_FC:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER; break;
    case AE_CH_LFE:   atChannel = CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY; break;
    case AE_CH_BL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT; break;
    case AE_CH_BR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT; break;
    case AE_CH_SL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT; break;
    case AE_CH_SR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT; break;
    case AE_CH_BC:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER; break;
    case AE_CH_FLOC:  atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER; break;
    case AE_CH_FROC:  atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER; break;
    default:          atChannel = CJNIAudioFormat::CHANNEL_INVALID; break;
  }
  return atChannel;
}

static CAEChannelInfo AUDIOTRACKChannelMaskToAEChannelMap(int atMask)
{
  CAEChannelInfo info;

  int mask = 0x1;
  for (unsigned int i = 0; i < sizeof(int32_t) * 8; i++)
  {
    if (atMask & mask)
      info += AUDIOTRACKChannelToAEChannel(mask);
    mask <<= 1;
  }

  return info;
}

static int AEChannelMapToAUDIOTRACKChannelMask(CAEChannelInfo info)
{
#ifdef LIMIT_TO_STEREO_AND_5POINT1_AND_7POINT1
  if (info.Count() > 6)
    return CJNIAudioFormat::CHANNEL_OUT_5POINT1
         | CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT
         | CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT;
  else if (info.Count() > 2)
    return CJNIAudioFormat::CHANNEL_OUT_5POINT1;
  else
    return CJNIAudioFormat::CHANNEL_OUT_STEREO;
#endif

  info.ResolveChannels(KnownChannels);

  int atMask = 0;

  for (unsigned int i = 0; i < info.Count(); i++)
    atMask |= AEChannelToAUDIOTRACKChannel(info[i]);

  return atMask;
}

jni::CJNIAudioTrack *CAESinkAUDIOTRACK::CreateAudioTrack(int stream, int sampleRate, int channelMask, int encoding, int bufferSize)
{
  jni::CJNIAudioTrack *jniAt = NULL;
  m_jniAudioFormat = encoding;

  try
  {
#if 0
    // Create Audiotrack per attribute
    if (CJNIAudioTrack::GetSDKVersion() >= 21)
    {
      CJNIAudioFormatBuilder fmtbuilder;
      fmtbuilder.setChannelMask(channelMask);
      fmtbuilder.setEncoding(encoding);
      fmtbuilder.setSampleRate(sampleRate);

      CJNIAudioAttributesBuilder attrbuilder;
      attrbuilder.setUsage(CJNIAudioAttributes::USAGE_MEDIA);
      attrbuilder.setContentType(CJNIAudioAttributes::CONTENT_TYPE_MOVIE);
      // Force direct output
      attrbuilder.setFlags(CJNIAudioAttributes::FLAG_HW_AV_SYNC);

      jniAt = new jni::CJNIAudioTrack(attrbuilder.build(),
                                 fmtbuilder.build(),
                                 bufferSize,
                                 CJNIAudioTrack::MODE_STREAM,
                                 0);
      if (m_jniAudioFormat == CJNIAudioFormat::ENCODING_PCM_FLOAT)
        m_jniBuffer = jharray(xbmc_jnienv()->NewFloatArray(bufferSizeInBytes / sizeof(float)));
      else
        m_jniBuffer = jharray(xbmc_jnienv()->NewByteArray(bufferSizeInBytes));
    
      m_buffer.setGlobal();
    }
    else
#endif
    {
      jniAt = new jni::CJNIAudioTrack(stream,
                                 sampleRate,
                                 channelMask,
                                 encoding,
                                 bufferSize,
                                 CJNIAudioTrack::MODE_STREAM);
    }
  }
  catch (const std::invalid_argument& e)
  {
    CLog::Log(LOGINFO, "AESinkAUDIOTRACK - AudioTrack creation (channelMask 0x%08x): %s", channelMask, e.what());
  }

  return jniAt;
}

int CAESinkAUDIOTRACK::AudioTrackWrite(char* audioData, int offsetInBytes, int sizeInBytes)
{
  int     written = 0;
  if (CJNIBase::GetSDKVersion() >= 21 && m_jniAudioFormat == CJNIAudioFormat::ENCODING_PCM_FLOAT)
  {
    std::vector<float> buf;
    buf.reserve(sizeInBytes / sizeof(float));
    memcpy(buf.data(), audioData + offsetInBytes, sizeInBytes / sizeof(float));
    written = m_at_jni->write(buf, 0, sizeInBytes / sizeof(float), CJNIAudioTrack::WRITE_NON_BLOCKING);
    written *= sizeof(float);
  }
  else if (m_jniAudioFormat == CJNIAudioFormat::ENCODING_IEC61937)
  {
    std::vector<int16_t> buf;
    buf.reserve(sizeInBytes / sizeof(int16_t));
    memcpy(buf.data(), audioData + offsetInBytes, sizeInBytes / sizeof(int16_t));
    if (CJNIBase::GetSDKVersion() >= 23)
      written = m_at_jni->write(buf, 0, sizeInBytes / sizeof(int16_t), CJNIAudioTrack::WRITE_NON_BLOCKING);
    else
      written = m_at_jni->write(buf, 0, sizeInBytes / sizeof(int16_t));
    written *= sizeof(uint16_t);
  }
  else
  {
    std::vector<char> buf;
    buf.reserve(sizeInBytes);
    memcpy(buf.data(), audioData + offsetInBytes, sizeInBytes);
    if (CJNIBase::GetSDKVersion() >= 23)
      written = m_at_jni->write(buf, 0, sizeInBytes, CJNIAudioTrack::WRITE_NON_BLOCKING);
    else
      written = m_at_jni->write(buf, 0, sizeInBytes);
  }
  
  return written;
}

int CAESinkAUDIOTRACK::AudioTrackWrite(char* audioData, int sizeInBytes, int64_t timestamp)
{
  int     written = 0;
  std::vector<char> buf;
  buf.reserve(sizeInBytes);
  memcpy(buf.data(), audioData, sizeInBytes);

  CJNIByteBuffer bytebuf = CJNIByteBuffer::wrap(buf);
  written = m_at_jni->write(bytebuf.get_raw(), sizeInBytes, CJNIAudioTrack::WRITE_BLOCKING, timestamp);

  return written;
}

std::set<unsigned int> CAESinkAUDIOTRACK::m_sink_sampleRates;

////////////////////////////////////////////////////////////////////////////////////////////
CAESinkAUDIOTRACK::CAESinkAUDIOTRACK()
{
  m_sink_frameSize = 0;
  m_audiotrackbuffer_sec = 0.0;
  m_at_jni = NULL;
  m_duration_written = 0;
  m_last_duration_written = 0;
  m_last_head_pos = 0;
  m_skip_delay = 0;
  m_sink_delay = 0;
  m_lastAddTimeMs = 0;
  m_head_pos_wrap_count = 0;
  m_head_pos_reset = 0;
}

CAESinkAUDIOTRACK::~CAESinkAUDIOTRACK()
{
  Deinitialize();
}

bool CAESinkAUDIOTRACK::IsSupported(int sampleRateInHz, int channelConfig, int encoding)
{
  int ret = CJNIAudioTrack::getMinBufferSize( sampleRateInHz, channelConfig, encoding);
  return (ret > 0);
}

bool CAESinkAUDIOTRACK::Initialize(AEAudioFormat &format, std::string &device)
{
  m_format      = format;
  m_volume      = -1;
  m_smoothedDelayCount = 0;
  m_smoothedDelayVec.clear();

  CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::Initialize requested: %p, sampleRate %u; format: %s(%d); channels: %d", this, format.m_sampleRate, CAEUtil::DataFormatToStr(format.m_dataFormat), format.m_dataFormat, format.m_channelLayout.Count());

  int stream = CJNIAudioManager::STREAM_MUSIC;

  if (AE_IS_RAW(m_format.m_dataFormat) && !CXBMCApp::IsHeadsetPlugged())
  {
    m_passthrough = true;

    // Get equal or lower supported sample rate
    std::set<unsigned int>::iterator s = m_sink_sampleRates.upper_bound((AE_IS_RAW_RAW(m_format.m_dataFormat) ? m_format.m_encodedRate : m_format.m_sampleRate));
    if (s != m_sink_sampleRates.begin())
      m_sink_sampleRate = *(--s);
    else
      m_sink_sampleRate = CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC);

    m_encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
//    m_sink_sampleRate       = CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC);

    switch (m_format.m_dataFormat)
    {
      case AE_FMT_AC3_RAW:
        if (CJNIAudioFormat::ENCODING_AC3 != -1)
        {
          m_encoding              = CJNIAudioFormat::ENCODING_AC3;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          m_format.m_frames       = m_format.m_sampleRate * 0.032;
        }
        else
          m_format.m_dataFormat   = AE_FMT_S16LE;
        break;

      case AE_FMT_EAC3_RAW:
        if (CJNIAudioFormat::ENCODING_E_AC3 != -1)
        {
          m_encoding              = CJNIAudioFormat::ENCODING_E_AC3;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          m_format.m_frames       = m_format.m_sampleRate * (1536.0 / m_format.m_encodedRate);
        }
        else
          m_format.m_dataFormat   = AE_FMT_S16LE;
        break;

      case AE_FMT_DTS_RAW:
        if (CJNIAudioFormat::ENCODING_DTS != -1)
        {
          m_encoding              = CJNIAudioFormat::ENCODING_DTS;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          m_format.m_frames       = m_format.m_sampleRate * (512.0 / m_format.m_encodedRate);
        }
        else
          m_format.m_dataFormat   = AE_FMT_S16LE;
        break;

      case AE_FMT_DTSHD_RAW:
        if (CJNIAudioFormat::ENCODING_DTS_HD != -1)
        {
          m_encoding              = CJNIAudioFormat::ENCODING_DTS_HD;
          m_format.m_channelLayout = AE_CH_LAYOUT_7_1;
          m_format.m_frames       = CONSTANT_BUFFER_SIZE_HD;
        }
        else
          m_format.m_dataFormat   = AE_FMT_S16LE;
        break;

      case AE_FMT_TRUEHD_RAW:
        if (CJNIAudioFormat::ENCODING_DOLBY_TRUEHD != -1)
        {
          m_encoding              = CJNIAudioFormat::ENCODING_DOLBY_TRUEHD;
          m_format.m_channelLayout = AE_CH_LAYOUT_7_1;
          m_format.m_frames       = CONSTANT_BUFFER_SIZE_HD;
        }
        else
          m_format.m_dataFormat   = AE_FMT_S16LE;
        break;

      case AE_FMT_AC3:
#ifdef HAS_LIBAMCODEC
        if (aml_present() && HasAmlHD())
          m_encoding              = CJNIAudioFormat::ENCODING_AC3;
        else
#endif
        {
          if (CJNIAudioFormat::ENCODING_IEC61937 != -1)
            m_encoding              = CJNIAudioFormat::ENCODING_IEC61937;
          else
            m_format.m_dataFormat   = AE_FMT_S16LE;
        }
        break;

      case AE_FMT_EAC3:
#ifdef HAS_LIBAMCODEC
        if (aml_present() && HasAmlHD())
        {
          m_encoding              = CJNIAudioFormat::ENCODING_E_AC3;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          m_sink_sampleRate       = 48000;
        }
        else
#endif
        {
          if (CJNIAudioFormat::ENCODING_IEC61937 != -1)
            m_encoding              = CJNIAudioFormat::ENCODING_IEC61937;
          else
            m_format.m_dataFormat   = AE_FMT_S16LE;
        }
        break;

      case AE_FMT_DTS:
#ifdef HAS_LIBAMCODEC
        if (aml_present() && HasAmlHD())
          m_encoding              = CJNIAudioFormat::ENCODING_DTS;
        else
#endif
        {
          if (CJNIAudioFormat::ENCODING_IEC61937 != -1)
            m_encoding              = CJNIAudioFormat::ENCODING_IEC61937;
          else
            m_format.m_dataFormat   = AE_FMT_S16LE;
        }
        break;

      case AE_FMT_DTSHD:
#ifdef HAS_LIBAMCODEC
        if (aml_present() && HasAmlHD())
          m_encoding              = CJNIAudioFormat::ENCODING_DTSHD_MA;
        else
#endif
        {
          if (CJNIAudioFormat::ENCODING_IEC61937 != -1)
            m_encoding              = CJNIAudioFormat::ENCODING_IEC61937;
          else
            m_format.m_dataFormat   = AE_FMT_S16LE;
        }
        break;

      case AE_FMT_TRUEHD:
#ifdef HAS_LIBAMCODEC
        if (aml_present() && HasAmlHD())
          m_encoding              = CJNIAudioFormat::ENCODING_TRUEHD;
        else
#endif
        {
          if (CJNIAudioFormat::ENCODING_IEC61937 != -1)
            m_encoding              = CJNIAudioFormat::ENCODING_IEC61937;
          else
            m_format.m_dataFormat   = AE_FMT_S16LE;
        }
        break;

      default:
        m_format.m_dataFormat   = AE_FMT_S16LE;
        break;
    }
  }
  else
  {
    m_passthrough = false;

    // Get equal or lower supported sample rate
    std::set<unsigned int>::iterator s = m_sink_sampleRates.upper_bound(m_format.m_sampleRate);
    if (s != m_sink_sampleRates.begin())
      m_sink_sampleRate = *(--s);
    else
      m_sink_sampleRate = CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC);

    m_format.m_sampleRate     = m_sink_sampleRate;
    if (CJNIAudioManager::GetSDKVersion() >= 21 && m_format.m_channelLayout.Count() == 2)
    {
      m_encoding = CJNIAudioFormat::ENCODING_PCM_FLOAT;
      m_format.m_dataFormat     = AE_FMT_FLOAT;
    }
    else
    {
      m_encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
      m_format.m_dataFormat     = AE_FMT_S16LE;
    }
  }

  int atChannelMask = AEChannelMapToAUDIOTRACKChannelMask(m_format.m_channelLayout);
  m_format.m_channelLayout  = AUDIOTRACKChannelMaskToAEChannelMap(atChannelMask);

#ifdef HAS_LIBAMCODEC
  if (m_passthrough && aml_present())
    atChannelMask = CJNIAudioFormat::CHANNEL_OUT_STEREO;
#endif

  while (!m_at_jni)
  {
    m_buffer_size = CJNIAudioTrack::getMinBufferSize( m_sink_sampleRate,
                                                      atChannelMask,
                                                      m_encoding);
    m_buffer_size *= 2;
    if (m_passthrough && !WantsIEC61937())
    {
      m_format.m_frameSize      = 1;
      m_sink_frameSize          = m_format.m_frameSize;
      m_buffer_size             = std::max((unsigned int) m_format.m_frames, m_buffer_size);
    }
    else
    {
      if (m_passthrough && m_format.m_sampleRate > 48000)
        m_buffer_size             = std::max((unsigned int) 65536, m_buffer_size);

      m_format.m_frameSize      = m_format.m_channelLayout.Count() *
                                    (CAEUtil::DataFormatToBits(m_format.m_dataFormat) / 8);
      m_sink_frameSize          = m_format.m_frameSize;
      m_format.m_frames       = (int)(m_buffer_size / m_format.m_frameSize) / 2;
    }

    m_format.m_frameSamples   = m_format.m_frames * m_format.m_channelLayout.Count();
    m_audiotrackbuffer_sec    = (double)(m_buffer_size / m_sink_frameSize) / (double)m_sink_sampleRate;

    m_at_jni                  = CreateAudioTrack(stream, m_sink_sampleRate,
                                                 atChannelMask, m_encoding,
                                                 m_buffer_size);

    if (!IsInitialized())
    {
      if (!m_passthrough)
      {
        if (atChannelMask != CJNIAudioFormat::CHANNEL_OUT_STEREO &&
            atChannelMask != CJNIAudioFormat::CHANNEL_OUT_5POINT1)
        {
          atChannelMask = CJNIAudioFormat::CHANNEL_OUT_5POINT1;
          CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - Retrying multichannel playback with a 5.1 layout");
          continue;
        }
        else if (atChannelMask != CJNIAudioFormat::CHANNEL_OUT_STEREO)
        {
          atChannelMask = CJNIAudioFormat::CHANNEL_OUT_STEREO;
          CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - Retrying with a stereo layout");
          continue;
        }
      }
      CLog::Log(LOGERROR, "AESinkAUDIOTRACK - Unable to create AudioTrack");
      Deinitialize();
      return false;
    }
    CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::Initialize returned: m_sampleRate %u; format:%s(%d); min_buffer_size %u; m_frames %u; m_frameSize %u; channels: %d; m_audiotrackbuffer_sec(%f), m_sink_saplerate(%d)", m_format.m_sampleRate, CAEUtil::DataFormatToStr(m_format.m_dataFormat), m_format.m_dataFormat, m_buffer_size, m_format.m_frames, m_format.m_frameSize, m_format.m_channelLayout.Count(), m_audiotrackbuffer_sec, m_sink_sampleRate);
  }

  format                    = m_format;

  // Force volume to 100% for passthrough
  if (m_passthrough && (WantsIEC61937() && m_encoding != CJNIAudioFormat::ENCODING_IEC61937))
  {
    CXBMCApp::get()->AcquireAudioFocus();
    m_volume = CXBMCApp::GetSystemVolume();
    CXBMCApp::SetSystemVolume(1.0);
  }

  return true;
}

void CAESinkAUDIOTRACK::Deinitialize()
{
  if (g_advancedSettings.CanLogComponent(LOGAUDIO))
    CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::Deinitialize %p", this);

  // Restore volume
  if (m_volume != -1)
  {
    CXBMCApp::SetSystemVolume(m_volume);
    CXBMCApp::get()->ReleaseAudioFocus();
  }

  if (!m_at_jni)
    return;

  if (IsInitialized())
  {
    CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::stopiing audiotrack");
    m_at_jni->stop();
    m_at_jni->flush();
  }
  m_at_jni->release();

  m_duration_written = 0;
  m_last_duration_written = 0;
  m_last_head_pos = 0;
  m_head_pos_wrap_count = 0;
  m_head_pos_reset = 0;
  m_skip_delay = 0;
  m_sink_delay = 0;
  m_lastAddTimeMs = 0;

  delete m_at_jni;
  m_at_jni = NULL;
}

bool CAESinkAUDIOTRACK::IsInitialized()
{
  return (m_at_jni && m_at_jni->getState() == CJNIAudioTrack::STATE_INITIALIZED);
}

void CAESinkAUDIOTRACK::GetDelay(AEDelayStatus& status)
{
  if (!m_at_jni)
  {
    status.SetDelay(0);
    return;
  }

  if (m_sink_delay && m_skip_delay < SKIP_DELAY_MAX && m_smoothedDelayCount >= SMOOTHED_DELAY_MAX)
  {
    status.SetDelay(m_sink_delay);
    m_skip_delay++;
    return;
  }
  else
    m_skip_delay = 0;

  uint64_t head_pos = 0;
  double frameDiffMilli = 0;
  if (CJNIBuild::SDK_INT >= 23)
  {
    CJNIAudioTimestamp ts;
    int64_t systime = CJNISystem::nanoTime();
    if (m_at_jni->getTimestamp(ts))
    {
      head_pos = ts.get_framePosition();
      frameDiffMilli = (systime - ts.get_nanoTime()) / 1000000000.0;

      if (g_advancedSettings.CanLogComponent(LOGAUDIO))
        CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::GetDelay timestamp: pos(%lld) time(%lld) diff(%f)", ts.get_framePosition(), ts.get_nanoTime(), frameDiffMilli);
    }
  }
  if (!head_pos)
  {
    // In their infinite wisdom, Google decided to make getPlaybackHeadPosition
    // return a 32bit "int" that you should "interpret as unsigned."  As such,
    // for wrap saftey, we need to do all ops on it in 32bit integer math.
    head_pos = (uint32_t)m_at_jni->getPlaybackHeadPosition();
    if (m_last_head_pos > head_pos)
    {
      // Wrapped
      m_head_pos_wrap_count++;
    }
    head_pos = head_pos + (m_head_pos_wrap_count << 32);
  }

  if (CJNIBuild::SDK_INT < 23 && (m_encoding == CJNIAudioFormat::ENCODING_AC3 || m_encoding == CJNIAudioFormat::ENCODING_E_AC3))
  {
    // According to exoplayer (https://github.com/google/ExoPlayer/blob/706c6908ec57005044f4b4a9f5b80266cad9eda3/library/src/main/java/com/google/android/exoplayer/audio/AudioTrack.java#L1159)
    // AC3 / EAC3 RAW pos can reset to zero when paused
    if (head_pos == 0 && m_at_jni->getPlayState() == CJNIAudioTrack::PLAYSTATE_PAUSED)
      m_head_pos_reset = m_last_head_pos;
  }
  head_pos += m_head_pos_reset;

  if (!head_pos)
  {
    status.SetDelay(m_duration_written);
    return;
  }

#if defined(HAS_LIBAMCODEC)
  if (aml_present() && (m_encoding == CJNIAudioFormat::ENCODING_E_AC3 || m_encoding == CJNIAudioFormat::ENCODING_DTSHD_MA || m_encoding == CJNIAudioFormat::ENCODING_TRUEHD))
    head_pos >>= 2;  // On 2 chans rather than 8
#endif

  double delay = m_duration_written - ((double)head_pos / m_sink_sampleRate);
  if (m_duration_written != m_last_duration_written && head_pos != m_last_head_pos)
  {
    m_smoothedDelayVec.push_back(delay - frameDiffMilli);
    if (m_smoothedDelayCount <= SMOOTHED_DELAY_MAX)
      m_smoothedDelayCount++;
    else
      m_smoothedDelayVec.erase(m_smoothedDelayVec.begin());

    m_last_duration_written = m_duration_written;
    m_last_head_pos = head_pos;
  }

  double smootheDelay = 0;
  for (double d : m_smoothedDelayVec)
    smootheDelay += d;
  smootheDelay /= m_smoothedDelayCount;

  if (g_advancedSettings.CanLogComponent(LOGAUDIO))
    CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::GetDelay m_duration_written/head_pos %f/%llu %f(%f)", m_duration_written, head_pos, smootheDelay, delay);

  status.SetDelay(smootheDelay);
  m_sink_delay = smootheDelay;
}

double CAESinkAUDIOTRACK::GetLatency()
{
  return 0.0;
}

double CAESinkAUDIOTRACK::GetCacheTotal()
{
  // total amount that the audio sink can buffer in units of seconds
  return m_audiotrackbuffer_sec;
}

// this method is supposed to block until all frames are written to the device buffer
// when it returns ActiveAESink will take the next buffer out of a queue
unsigned int CAESinkAUDIOTRACK::AddPackets(uint8_t **data, unsigned int frames, unsigned int offset)
{
  if (!IsInitialized())
    return INT_MAX;

  uint8_t *buffer = data[0]+offset*m_format.m_frameSize;
  uint8_t *out_buf = buffer;
  int size = frames * m_format.m_frameSize;

  if (m_passthrough && !WantsIEC61937())
  {
    // Decapsulate
    size = ((int*)(buffer))[0];
    out_buf = buffer + sizeof(int);
    if (!size)
    {
      if (m_at_jni->getPlayState() == CJNIAudioTrack::PLAYSTATE_PLAYING)
        m_at_jni->pause();
      return frames;
    }
  }

  // write as many frames of audio as we can fit into our internal buffer.
  int written = 0;
  double duration = (double)frames / m_format.m_sampleRate;
  if (frames)
  {
    // android will auto pause the playstate when it senses idle,
    // check it and set playing if it does this. Do this before
    // writing into its buffer.
    if (m_at_jni->getPlayState() != CJNIAudioTrack::PLAYSTATE_PLAYING)
      m_at_jni->play();
    unsigned int toWrite = size;
    while (toWrite > 0)
    {
      int bsize = std::min(toWrite, m_buffer_size);
      int len = AudioTrackWrite((char*)(&out_buf[written]), 0, bsize);
      // int len = m_at_jni->write((char*)(&out_buf[written]), bsize, (int64_t)(m_duration_written * 1000000));  // by timestamp
      if (len < 0)
      {
        CLog::Log(LOGERROR, "CAESinkAUDIOTRACK::AddPackets write returned error:  %d(%d)", len, written);
        return INT_MAX;
      }
      written += len;
      toWrite -= len;
      if (toWrite)
      {
        double toSleep = duration / 2;
//        CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::AddPackets leftovers(%d), sleeping(%f)", toWrite, toSleep);
        usleep(toSleep * 1000000);
      }
    }
    written = frames * m_format.m_frameSize;     // Be sure to report to AE everything has been written

    m_duration_written += duration;

    int32_t diff = 0;
    int32_t sleep_ms = 0;
//    if (m_passthrough && !WantsIEC61937())
//    {
//      if (!m_lastAddTimeMs)
//        m_lastAddTimeMs = XbmcThreads::SystemClockMillis();
//      diff = XbmcThreads::SystemClockMillis() - m_lastAddTimeMs;
//      sleep_ms = (duration * 1000.0) - diff;
//      if (sleep_ms > 0)
//        usleep(sleep_ms * 1000.0);
//    }

    if (g_advancedSettings.CanLogComponent(LOGAUDIO))
      CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::AddPackets written %d(%d), tm:%d(%d;%d)", written, size, XbmcThreads::SystemClockMillis() - m_lastAddTimeMs, diff, sleep_ms);

    m_lastAddTimeMs = XbmcThreads::SystemClockMillis();
  }
  return (unsigned int)(written/m_format.m_frameSize);
}

void CAESinkAUDIOTRACK::Drain()
{
  if (!m_at_jni)
    return;

  // TODO: does this block until last samples played out?
  // we should not return from drain as long the device is in playing state
  m_at_jni->stop();
  m_duration_written = 0;
  m_last_duration_written = 0;
  m_last_head_pos = 0;
  m_head_pos_wrap_count = 0;
  m_head_pos_reset = 0;
  m_sink_delay = 0;
  m_lastAddTimeMs = 0;
}

bool CAESinkAUDIOTRACK::WantsIEC61937()
{
  return !(AE_IS_RAW_RAW(m_format.m_dataFormat));
}

bool CAESinkAUDIOTRACK::HasAmlHD()
{
  return (CJNIAudioFormat::ENCODING_TRUEHD != -1);
}

void CAESinkAUDIOTRACK::EnumerateDevicesEx(AEDeviceInfoList &list, bool force)
{
  // Enumerate audio devices on API >= 23
  if (CJNIAudioManager::GetSDKVersion() >= 23)
  {
      // Warning: getDevices has a race with HDMI enumeration after a refresh rate switch
      /*
      CJNIAudioManager audioManager(CJNIContext::getSystemService("audio"));
      CJNIAudioDeviceInfos devices = audioManager.getDevices(CJNIAudioManager::GET_DEVICES_OUTPUTS);

      for (auto dev : devices)
      {
        CLog::Log(LOGDEBUG, "--- Found device: %s", dev.getProductName().toString().c_str());
        CLog::Log(LOGDEBUG, "    id: %d, type: %d, isSink: %s, isSource: %s", dev.getId(), dev.getType(), dev.isSink() ? "true" : "false", dev.isSource() ? "true" : "false");

        std::ostringstream oss;
        for (auto i : dev.getChannelCounts())
          oss << i << " / ";
        CLog::Log(LOGDEBUG, "    channel counts: %s", oss.str().c_str());

        oss.clear(); oss.str("");
        for (auto i : dev.getChannelIndexMasks())
          oss << i << " / ";
        CLog::Log(LOGDEBUG, "    channel index masks: %s", oss.str().c_str());

        oss.clear(); oss.str("");
        for (auto i : dev.getChannelMasks())
          oss << i << " / ";
        CLog::Log(LOGDEBUG, "    channel masks: %s", oss.str().c_str());

        oss.clear(); oss.str("");
        for (auto i : dev.getEncodings())
          oss << i << " / ";
        CLog::Log(LOGDEBUG, "    encodings: %s", oss.str().c_str());

        oss.clear(); oss.str("");
        for (auto i : dev.getSampleRates())
          oss << i << " / ";
        CLog::Log(LOGDEBUG, "    sample rates: %s", oss.str().c_str());
      }
      */
  }

  m_sink_sampleRates.clear();
  m_sink_sampleRates.insert(CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC));

  int test_sample[] = { 32000, 44100, 48000, 96000, 192000 };
  int test_sample_sz = sizeof(test_sample) / sizeof(int);
  int encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
  if (CJNIAudioManager::GetSDKVersion() >= 21)
    encoding = CJNIAudioFormat::ENCODING_PCM_FLOAT;
  for (int i=0; i<test_sample_sz; ++i)
  {
    if (IsSupported(test_sample[i], CJNIAudioFormat::CHANNEL_OUT_STEREO, encoding))
    {
      m_sink_sampleRates.insert(test_sample[i]);
      CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - %d supported", test_sample[i]);
    }
  }

  CAEDeviceInfo pcminfo;
  pcminfo.m_channels.Reset();
  pcminfo.m_dataFormats.clear();
  pcminfo.m_sampleRates.clear();

  pcminfo.m_deviceName = "AudioTrackPCM";
  pcminfo.m_displayName = "Android";
  pcminfo.m_displayNameExtra = "PCM";

  pcminfo.m_deviceType = AE_DEVTYPE_PCM;
#ifdef LIMIT_TO_STEREO_AND_5POINT1_AND_7POINT1
  pcminfo.m_channels = AE_CH_LAYOUT_7_1;
#else
  m_info.m_channels = KnownChannels;
#endif
  pcminfo.m_dataFormats.push_back(AE_FMT_S16LE);
  if (CJNIAudioManager::GetSDKVersion() >= 21)
    pcminfo.m_dataFormats.push_back(AE_FMT_FLOAT);

  std::copy(m_sink_sampleRates.begin(), m_sink_sampleRates.end(), std::back_inserter(pcminfo.m_sampleRates));
  list.push_back(pcminfo);

  if (!CXBMCApp::IsHeadsetPlugged())
  {
    CAEDeviceInfo ptinfo = pcminfo;
    ptinfo.m_dataFormats.clear();
    ptinfo.m_sampleRates.clear();

    ptinfo.m_deviceName = "AudioTrackPT";
    ptinfo.m_displayName = "Android";
    ptinfo.m_displayNameExtra = "IEC Passthrough";

    ptinfo.m_deviceType = AE_DEVTYPE_HDMI;
    // passthrough
    if (CJNIAudioFormat::ENCODING_IEC61937 != -1)
      CLog::Log(LOGINFO, "AESinkAUDIOTRACK - ENCODING_IEC61937 detected");

    m_sink_sampleRates.insert(44100);
    m_sink_sampleRates.insert(48000);
    ptinfo.m_dataFormats.push_back(AE_FMT_AC3);
    ptinfo.m_dataFormats.push_back(AE_FMT_DTS);
    if (HasAmlHD() || CJNIAudioFormat::ENCODING_IEC61937 != -1)
    {
      m_sink_sampleRates.insert(192000);   // For HD audio
      ptinfo.m_dataFormats.push_back(AE_FMT_EAC3);
      ptinfo.m_dataFormats.push_back(AE_FMT_TRUEHD);
      ptinfo.m_dataFormats.push_back(AE_FMT_DTSHD);
    }
    std::copy(m_sink_sampleRates.begin(), m_sink_sampleRates.end(), std::back_inserter(ptinfo.m_sampleRates));

    if (CJNIBase::GetSDKVersion() >= 21)
    {
      CAEDeviceInfo rawptinfo = ptinfo;
      rawptinfo.m_dataFormats.clear();
      rawptinfo.m_sampleRates.clear();

      rawptinfo.m_deviceName = "AudioTrackPTRAW";
      rawptinfo.m_displayName = "Android";
      rawptinfo.m_displayNameExtra = "RAW Passthrough";

      if (CJNIAudioFormat::ENCODING_AC3 != -1)
        rawptinfo.m_dataFormats.push_back(AE_FMT_AC3_RAW);
      else
        rawptinfo.m_dataFormats.push_back(AE_FMT_AC3);
      if (CJNIAudioFormat::ENCODING_E_AC3 != -1)
        rawptinfo.m_dataFormats.push_back(AE_FMT_EAC3_RAW);
      if (CJNIAudioFormat::ENCODING_DTS != -1)
        rawptinfo.m_dataFormats.push_back(AE_FMT_DTS_RAW);
      else
        rawptinfo.m_dataFormats.push_back(AE_FMT_DTS);
      if (CJNIAudioFormat::ENCODING_DTS_HD != -1)
        rawptinfo.m_dataFormats.push_back(AE_FMT_DTSHD_RAW);
      if (CJNIAudioFormat::ENCODING_DOLBY_TRUEHD != -1)
        rawptinfo.m_dataFormats.push_back(AE_FMT_TRUEHD_RAW);

      std::copy(m_sink_sampleRates.begin(), m_sink_sampleRates.end(), std::back_inserter(rawptinfo.m_sampleRates));

      if (CJNIAudioFormat::ENCODING_DOLBY_TRUEHD != -1)  // Shield
      {
        list.push_back(rawptinfo);
        list.push_back(ptinfo);
      }
      else
      {
        list.push_back(ptinfo);
        list.push_back(rawptinfo);
      }
    }
    else
    {
      list.push_back(ptinfo);
    }
  }
}

