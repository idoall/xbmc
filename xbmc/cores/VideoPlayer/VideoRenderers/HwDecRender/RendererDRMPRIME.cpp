/*
 *      Copyright (C) 2017 Team Kodi
 *      http://kodi.tv
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
 *  along with this Program; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "RendererDRMPRIME.h"

#include "cores/VideoPlayer/VideoRenderers/RenderCapture.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"
#include "windowing/gbm/DRMUtils.h"

CRendererDRMPRIME::CRendererDRMPRIME()
{
}

CRendererDRMPRIME::~CRendererDRMPRIME()
{
  Reset();
}

CBaseRenderer* CRendererDRMPRIME::Create(CVideoBuffer* buffer)
{
  if (buffer && dynamic_cast<CVideoBufferDRMPRIME*>(buffer))
    return new CRendererDRMPRIME();

  return nullptr;
}

bool CRendererDRMPRIME::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("drm_prime", CRendererDRMPRIME::Create);
  return true;
}

bool CRendererDRMPRIME::Configure(const VideoPicture& picture, float fps, unsigned flags, unsigned int orientation)
{
  m_format = picture.videoBuffer->GetFormat();
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  // Save the flags.
  m_iFlags = flags;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  Reset();

  m_bConfigured = true;
  return true;
}

void CRendererDRMPRIME::AddVideoPicture(const VideoPicture& picture, int index, double currentClock)
{
  BUFFER& buf = m_buffers[index];
  buf.videoBuffer = picture.videoBuffer;
  buf.videoBuffer->Acquire();
}

void CRendererDRMPRIME::Reset()
{
  for (int i = 0; i < m_numRenderBuffers; i++)
    ReleaseBuffer(i);

  m_iLastRenderBuffer = -1;
}

void CRendererDRMPRIME::ReleaseBuffer(int index)
{
  BUFFER& buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    CVideoBufferDRMPRIME* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(buf.videoBuffer);
    if (buffer)
      buffer->Release();
    buf.videoBuffer = nullptr;
  }
}

bool CRendererDRMPRIME::NeedBuffer(int index)
{
  return m_iLastRenderBuffer == index;
}

CRenderInfo CRendererDRMPRIME::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_numRenderBuffers;
  info.optimal_buffer_size = m_numRenderBuffers;
  info.opaque_pointer = (void*)this;
  return info;
}

void CRendererDRMPRIME::Update()
{
  if (!m_bConfigured)
    return;

  ManageRenderArea();
}

void CRendererDRMPRIME::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  if (m_iLastRenderBuffer == index)
    return;

  CVideoBufferDRMPRIME* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(m_buffers[index].videoBuffer);
  if (buffer)
    SetVideoPlane(buffer);

  m_iLastRenderBuffer = index;
}

bool CRendererDRMPRIME::RenderCapture(CRenderCapture* capture)
{
  capture->BeginRender();
  capture->EndRender();
  return true;
}

bool CRendererDRMPRIME::ConfigChanged(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

  return false;
}

bool CRendererDRMPRIME::Supports(ERENDERFEATURE feature)
{
  if (feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_PIXEL_RATIO)
    return true;

  return false;
}

bool CRendererDRMPRIME::Supports(ESCALINGMETHOD method)
{
  return false;
}

void CRendererDRMPRIME::SetVideoPlane(CVideoBufferDRMPRIME* buffer)
{
  AVDRMFrameDescriptor* descriptor = buffer->GetDescriptor();
  if (descriptor && descriptor->nb_layers)
  {
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    struct drm* drm = CDRMUtils::GetDrm();
    int ret;

    // convert Prime FD to GEM handle
    for (int object = 0; object < descriptor->nb_objects; object++)
    {
      ret = drmPrimeFDToHandle(drm->fd, descriptor->objects[object].fd, &buffer->m_handles[object]);
      if (ret < 0)
      {
        CLog::Log(LOGERROR, "CRendererDRMPRIME::%s - failed to retrieve the GEM handle from prime fd %d, ret = %d", __FUNCTION__, descriptor->objects[object].fd, ret);
        return;
      }
    }

    AVDRMLayerDescriptor* layer = &descriptor->layers[0];

    for (int plane = 0; plane < layer->nb_planes; plane++)
    {
      uint32_t handle = buffer->m_handles[layer->planes[plane].object_index];
      if (handle && layer->planes[plane].pitch)
      {
        handles[plane] = handle;
        pitches[plane] = layer->planes[plane].pitch;
        offsets[plane] = layer->planes[plane].offset;
      }
    }

    // add the video frame FB
    ret = drmModeAddFB2(drm->fd, buffer->GetWidth(), buffer->GetHeight(), layer->format, handles, pitches, offsets, &buffer->m_fb_id, 0);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CRendererDRMPRIME::%s - failed to add drm layer %d, ret = %d", __FUNCTION__, buffer->m_fb_id, ret);
      return;
    }

    int32_t crtc_x = (int32_t)m_destRect.x1;
    int32_t crtc_y = (int32_t)m_destRect.y1;
    uint32_t crtc_w = (uint32_t)m_destRect.Width();
    uint32_t crtc_h = (uint32_t)m_destRect.Height();
    uint32_t src_x = 0;
    uint32_t src_y = 0;
    uint32_t src_w = buffer->GetWidth() << 16;
    uint32_t src_h = buffer->GetHeight() << 16;

    // TODO: use atomic or legacy api

    // show the video frame FB on the video plane
    ret = drmModeSetPlane(drm->fd, drm->video_plane_id, drm->crtc_id, buffer->m_fb_id, 0,
                          crtc_x, crtc_y, crtc_w, crtc_h,
                          src_x, src_y, src_w, src_h);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CRendererDRMPRIME::%s - failed to set drm plane %d, buffer = %d, ret = %d", __FUNCTION__, drm->video_plane_id, buffer->m_fb_id, ret);
      return;
    }
  }
}
