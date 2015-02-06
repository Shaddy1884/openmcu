
#include <ptlib.h>

#include "mcu.h"

#ifdef _WIN32
  //fcntl.h
# define FD_CLOEXEC     1       /* posix */
# define F_DUPFD        0       /* Duplicate fildes */
# define F_GETFD        1       /* Get fildes flags (close on exec) */
# define F_SETFD        2       /* Set fildes flags (close on exec) */
# define F_GETFL        3       /* Get file flags */
# define F_SETFL        4       /* Set file flags */
# define O_NONBLOCK     0x4000
  inline int fcntl (int, int, ...) {return -1;}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL PreParseMsg(PString & msg_str)
{
  if(msg_str.Find("Cseq:") != P_MAX_INDEX)
    msg_str.Replace("Cseq:","CSeq:",TRUE,0);

  if(msg_str.Find("CSeq:") == P_MAX_INDEX)
  {
    MCUTRACE(1, "RTSP pre-parse: CSeq header not found");
    return FALSE;
  }

  // create not empty CSeq method name
  PString method_name = "EMPTY";
  PString cseq;
  for(PINDEX i = msg_str.Find("CSeq:")+6; i < msg_str.GetLength(); i++)
  {
    if(msg_str[i] == ' ' || msg_str[i] == '\r')
      break;
    cseq += msg_str[i];
  }
  if(cseq == "")
  {
    MCUTRACE(1, "RTSP pre-parse: CSeq number not found");
    return FALSE;
  }
  msg_str.Replace("CSeq: "+cseq, "CSeq: "+cseq+" "+method_name, TRUE, 0);

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

msg_t * ParseMsg(PString & msg_str)
{
  if(PreParseMsg(msg_str) == FALSE)
    return NULL;

  msg_t *msg = msg_make(sip_default_mclass(), 0, (const void *)(const char *)msg_str, msg_str.GetLength());
  sip_t *sip = sip_object(msg);
  if(sip == NULL || sip->sip_cseq == NULL)
  {
    MCUTRACE(1, "RTSP parse: failed parse rtsp message");
    goto error;
  }
  if(sip->sip_request == NULL && sip->sip_status == NULL)
  {
    MCUTRACE(1, "RTSP parse: failed parse rtsp message");
    goto error;
  }

  return msg;

  error:
    msg_destroy(msg);
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void ConferenceStreamMember::Close()
{
  MCUH323EndPoint & ep = OpenMCU::Current().GetEndpoint();
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if(conn != NULL)
  {
    conn->ClearCall();
    conn->Unlock();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCURtspConnection::MCURtspConnection(MCUH323EndPoint *_ep, PString _callToken)
  :MCUSipConnection(_ep, _callToken)
{
  connectionType = CONNECTION_TYPE_RTSP;
  trace_section = "RTSP Connection "+callToken+": ";
  remoteApplication = "RTSP terminal";

  rtsp_state = RTSP_NONE;
  cseq = 1;
  listener = NULL;

  // create local capability list
  CreateLocalSipCaps();

  OnCreated();

  PTRACE(1, trace_section << "constructor");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCURtspConnection::~MCURtspConnection()
{
  PTRACE(1, trace_section << "destructor");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::ClearCall(H323Connection::CallEndReason reason)
{
  return MCUH323Connection::ClearCall(reason);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspConnection::CleanUpOnCallEnd()
{
  PTRACE(1, trace_section << "CleanUpOnCallEnd reason: " << callEndReason);

  connectionState = ShuttingDownConnection;

  if(direction == DIRECTION_OUTBOUND && callEndReason == EndedByLocalUser && rtsp_state == RTSP_PLAYING)
    SendTeardown();

  if(listener)
    delete listener;
  listener = NULL;

  StopTransmitChannels();
  StopReceiveChannels();
  DeleteChannels();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::Connect(PString room, PString address)
{
  direction = DIRECTION_OUTBOUND;

  // requested room
  requestedRoom = room;

  // rtsp address (remote)
  address.Replace("rtsp:","http:",TRUE,0);
  MCUURL url(address);
  PString rtsp_port = url.GetPort();
  if(rtsp_port == "80") rtsp_port = "554";
  ruri_str = "rtsp://"+url.GetHostName()+":"+rtsp_port+url.GetPathStr();
  remotePartyAddress = ruri_str;

  // detect local_ip, nat_ip and create rtp sessions
  if(CreateDefaultRTPSessions() == FALSE)
    goto error;

  // display name
  remotePartyName = GetEndpointParam(DisplayNameKey, url.GetPathStr());

  // auth
  auth_username = GetEndpointParam(UserNameKey, url.GetUserName());
  auth_password = GetEndpointParam(PasswordKey, url.GetPassword());

  // create listener
  listener = MCUListener::Create(MCU_LISTENER_TCP_CLIENT, url.GetHostName(), rtsp_port.AsInteger(), OnReceived_wrap, this);
  if(listener == NULL)
    goto error;

  // start connection
  if(SendDescribe() == 0)
    return FALSE;

  return TRUE;

  error:
    ClearCall();
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::Connect(MCUSocket *socket, const msg_t *msg)
{
  direction = DIRECTION_INBOUND;

  // remote address proto:host:port
  ruri_str = socket->GetAddress();

  sip_t *sip = sip_object(msg);

  // random session string
  rtsp_session_str = PString(random());

  // rtsp address (local)
  luri_str = url_as_string(msg_home(msg), sip->sip_request->rq_url);
  MCUURL lurl(luri_str);
  rtsp_path = lurl.GetPath()[0];

  // set remote application
  if(sip->sip_user_agent)
    remoteApplication = sip->sip_user_agent->g_string;
  else if(sip->sip_server)
    remoteApplication = sip->sip_server->g_string;

  // used in GetEndpointParam
  remotePartyAddress = "RTSP Server "+rtsp_path;

  // requested room
  requestedRoom = GetEndpointParam(RoomNameKey);

  // detect local_ip, nat_ip and create rtp sessions
  if(!CreateDefaultRTPSessions())
    goto error;

  if(!CreateInboundCaps())
    goto error;

  // auth
  auth_username = GetEndpointParam(UserNameKey);
  auth_password = GetEndpointParam(PasswordKey);
  if(auth_password != "")
  {
    auth_type = AUTH_WWW;
    auth_scheme = "Digest";
    auth_realm = "openmcu-ru";
    auth_nonce = PGloballyUniqueID().AsString();
  }

  // create listener
  listener = MCUListener::Create(MCU_LISTENER_TCP_CLIENT, socket, OnReceived_wrap, this);
  if(listener == NULL)
    goto error;

  // start connection
  OnRequestReceived(msg);

  return TRUE;

  error:
    ClearCall();
    return FALSE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspConnection::CreateLocalSipCaps()
{
  LocalSipCaps.clear();
  for(SipCapMapType::iterator it = sep->GetBaseSipCaps().begin(); it != sep->GetBaseSipCaps().end(); ++it)
  {
    SipCapability *base_sc = it->second;
    if(SkipCapability(base_sc->capname, connectionType))
      continue;
    LocalSipCaps.insert(SipCapMapType::value_type(it->first, new SipCapability(*base_sc)));
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::CreateInboundCaps()
{
  // create remote caps, for RTP channels
  for(SipCapMapType::iterator it = LocalSipCaps.begin(); it != LocalSipCaps.end(); ++it)
  {
    SipCapability *local_sc = it->second;
    RemoteSipCaps.insert(SipCapMapType::value_type(it->first, new SipCapability(*local_sc)));
  }

  PString audio_codec = GetEndpointParam(AudioCodecKey);
  PString video_codec = GetEndpointParam(VideoCodecKey);
  PString video_resolution = GetEndpointParam(VideoResolutionKey, "352x288");
  unsigned video_bandwidth = GetEndpointParam(BandwidthFromKey, "256").AsInteger();
  unsigned video_frame_rate = GetEndpointParam(FrameRateFromKey, "10").AsInteger();

  unsigned video_width = video_resolution.Tokenise("x")[0].AsInteger();
  unsigned video_height = video_resolution.Tokenise("x")[1].AsInteger();

  // setup audio capability
  SipCapability *audio_sc = FindSipCap(RemoteSipCaps, audio_codec);
  if(audio_sc)
  {
    audio_sc->cap = MCUCapability::Create(audio_codec);
    if(audio_sc->cap == NULL)
    {
      MCUTRACE(1, trace_section << "not found audio codec " << audio_codec);
      return FALSE;
    }
    if(audio_sc->payload == -1)
      audio_sc->payload = 96;
    scap = audio_sc->payload;
  }
  // setup video capability
  SipCapability *video_sc = FindSipCap(RemoteSipCaps, video_codec);
  if(video_sc)
  {
    video_sc->cap = MCUCapability::Create(video_codec);
    if(video_sc->cap == NULL)
    {
      MCUTRACE(1, trace_section << "not found video codec " << video_codec);
      return FALSE;
    }
    if(video_sc->payload == -1)
      video_sc->payload = 97;
    vcap = video_sc->payload;
    video_sc->video_width = video_width;
    video_sc->video_height = video_height;
    video_sc->video_frame_rate = video_frame_rate;
    video_sc->bandwidth = video_bandwidth;
    //sc->fmtp = "profile-level-id=1;";
    video_sc->fmtp = "";
    OpalMediaFormat & wf = video_sc->cap->GetWritableMediaFormat();
    SetFormatParams(wf, video_sc->video_width, video_sc->video_height, video_sc->video_frame_rate, video_sc->bandwidth);
    //int keyint = ((video_frame_rate+4)*2/10)*10;
    //wf.SetOptionInteger(OPTION_TX_KEY_FRAME_PERIOD, keyint);
  }

  if(audio_sc == NULL && video_sc == NULL)
  {
    MCUTRACE(1, trace_section << "cannot create connection without codecs, audio: " << audio_codec << " video: " << video_codec);
    return FALSE;
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::SendOptions()
{
  char buffer[1024];
  snprintf(buffer, 1024,
  	   "OPTIONS %s RTSP/1.0\r\n"
	   "CSeq: %d %s\r\n"
	   , (const char *)ruri_str, cseq++, (const char *)METHOD_OPTIONS);

  AddHeaders(buffer, METHOD_OPTIONS);
  if(!SendRequest(buffer))
    return FALSE;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::SendPlay()
{
  char buffer[1024];
  snprintf(buffer, 1024,
  	   "PLAY %s RTSP/1.0\r\n"
	   "CSeq: %d %s\r\n"
           "Session: %s\r\n"
           "Range: npt=0.000-\r\n"
	   , (const char *)ruri_str, cseq++, (const char *)METHOD_PLAY, (const char *)rtsp_session_str);

  AddHeaders(buffer, METHOD_PLAY);
  if(!SendRequest(buffer))
    return FALSE;

  rtsp_state = RTSP_PLAY;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::SendSetup(int pt)
{
  SipCapability *sc = FindSipCap(RemoteSipCaps, pt);
  if(sc->attr.GetAt("control") == NULL)
  {
    MCUTRACE(1, trace_section << "capability attribute \"control\" not found");
    ClearCall();
    return FALSE;
  }
  PString control = sc->attr("control");
  if(control.Left(4) != "rtsp")
    control = ruri_str+"/"+control;

  unsigned rtp_port = 0;
  if(pt == scap)
    rtp_port = audio_rtp_port;
  else
    rtp_port = video_rtp_port;

  PString session_header;
  if(rtsp_session_str != "")
    session_header = "Session: "+rtsp_session_str+"\r\n";

  char buffer[1024];
  snprintf(buffer, 1024,
  	   "SETUP %s RTSP/1.0\r\n"
	   "CSeq: %d %s\r\n"
	   "%s"
           "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n"
	   , (const char *)control, cseq++, (const char *)METHOD_SETUP, (const char *)session_header, rtp_port, rtp_port+1);

  AddHeaders(buffer, METHOD_SETUP);
  if(!SendRequest(buffer))
    return FALSE;

  if(pt == scap)
    rtsp_state = RTSP_SETUP_AUDIO;
  else
    rtsp_state = RTSP_SETUP_VIDEO;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::SendTeardown()
{
  char buffer[1024];
  snprintf(buffer, 1024,
  	   "TEARDOWN %s RTSP/1.0\r\n"
	   "CSeq: %d %s\r\n"
	   "Session: %s\r\n"
	   , (const char *)ruri_str, cseq++, (const char *)METHOD_TEARDOWN, (const char *)rtsp_session_str);

  AddHeaders(buffer, METHOD_TEARDOWN);
  if(!SendRequest(buffer))
    return FALSE;

  rtsp_state = RTSP_TEARDOWN;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::SendDescribe()
{
  char buffer[1024];
  snprintf(buffer,1024,
  	   "DESCRIBE %s RTSP/1.0\r\n"
	   "CSeq: %d %s\r\n"
	   "Accept: application/sdp\r\n"
	   , (const char *)ruri_str, cseq++, (const char *)METHOD_DESCRIBE);

  AddHeaders(buffer, METHOD_DESCRIBE);
  if(!SendRequest(buffer))
     return FALSE;

  rtsp_state = RTSP_DESCRIBE;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnResponsePlay(const msg_t *msg)
{
  // set endpoint member name
  SetMemberName();
  // override requested room from registrar
  SetRequestedRoom();
  // join conference
  OnEstablished();
  if(!conference || !conferenceMember || !conferenceMember->IsJoined())
  {
    MCUTRACE(1, trace_section << "error");
    return FALSE;
  }

  // create and start channels
  CreateMediaChannel(scap, 0);
  CreateMediaChannel(vcap, 0);
  StartReceiveChannels();

  // is connected
  connectionState = EstablishedConnection;

  if(scap > 0)
  {
    SipCapability *sc = FindSipCap(RemoteSipCaps, scap);
    MCUTRACE(1, trace_section << "audio " << sc->capname << " " << sc->remote_ip << ":" << sc->remote_port);
  }
  if(vcap > 0)
  {
    SipCapability *sc = FindSipCap(RemoteSipCaps, vcap);
    MCUTRACE(1, trace_section << "video " << sc->capname << " " << sc->remote_ip << ":" << sc->remote_port);
  }

  rtsp_state = RTSP_PLAYING;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnResponseSetup(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);

  PString transport_str;
  for(sip_unknown_t *sip_un = sip->sip_unknown; sip_un != NULL; sip_un = sip_un->un_next)
  {
    if(PString(sip_un->un_name) == "Session")
      rtsp_session_str = PString(sip_un->un_value).Tokenise(";")[0];
    if(PString(sip_un->un_name) == "Transport")
      transport_str = sip_un->un_value;
  }

  SipCapability *sc = NULL;
  if(rtsp_state == RTSP_SETUP_AUDIO)
    sc = FindSipCap(RemoteSipCaps, scap);
  else
    sc = FindSipCap(RemoteSipCaps, vcap);

  if(ParseTransportStr(sc, transport_str) == FALSE)
  {
    MCUTRACE(1, trace_section << "failed parse transport header");
    return FALSE;
  }

  if(rtsp_state == RTSP_SETUP_AUDIO && vcap > 0)
  {
    SendSetup(vcap);
    return TRUE;
  }

  SendPlay();
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnResponseDescribe(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);

  if(!sip->sip_payload || !sip->sip_payload->pl_data)
  {
    MCUTRACE(1, trace_section << "error");
    return FALSE;
  }

  PString sdp_str = sip->sip_payload->pl_data;
  int response_code = ProcessSDP(LocalSipCaps, RemoteSipCaps, sdp_str);
  if(response_code)
  {
    MCUTRACE(1, trace_section << "error");
    return FALSE;
  }

  // set remote application
  if(sip->sip_user_agent)
    remoteApplication = sip->sip_user_agent->g_string;
  else if(sip->sip_server)
    remoteApplication = sip->sip_server->g_string;

  if(scap >= 0)
    SendSetup(scap);
  else
    SendSetup(vcap);

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnRequestOptions(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);

  char buffer[1024];
  snprintf(buffer, 1024,
  	   "RTSP/1.0 200 OK\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString());

  AddHeaders(buffer);
  if(SendRequest(buffer) == 0)
     return FALSE;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnRequestDescribe(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);

  char buffer_sdp[1024];
  snprintf(buffer_sdp, 1024,
           "v=0\r\n"
           "o=- 15516361289475271524 15516361289475271524 IN IP4 OpenMCU-ru\r\n"
           "s=Unnamed\r\n"
           "i=N/A\r\n"
           "c=IN IP4 0.0.0.0\r\n"
           "t=0 0\r\n"
           "a=recvonly\r\n"
           "a=type:unicast\r\n"
           "a=charset:UTF-8\r\n"
           "a=control:%s\r\n"
           , (const char *)luri_str);

  if(scap >= 0)
  {
    SipCapability *sc = FindSipCap(RemoteSipCaps, scap);
    if(sc)
    {
      snprintf(buffer_sdp + strlen(buffer_sdp), 1024,
           "m=audio 0 RTP/AVP %d\r\n"
           "a=rtpmap:%d %s/%d%s\r\n"
           "a=control:%s\r\n"
           , sc->payload, sc->payload, (const char *)sc->format.ToUpper(), sc->clock, (const char *)(sc->params == "" ? "" : "/"+sc->params)
           , (const char *)(luri_str+"/audio"));
    }
  }

  if(vcap >= 0)
  {
    SipCapability *sc = FindSipCap(RemoteSipCaps, vcap);
    if(sc)
    {
      snprintf(buffer_sdp + strlen(buffer_sdp), 1024,
           "m=video 0 RTP/AVP %d\r\n"
           "b=AS:%d\r\n"
           "a=rtpmap:%d %s/90000\r\n"
           "a=fmtp:%d %s\r\n"
           "a=control:%s\r\n"
           , sc->payload, sc->bandwidth, sc->payload, (const char *)sc->format.ToUpper()
           , sc->payload, (const char *)sc->fmtp
           , (const char *)(luri_str+"/video"));
    }
  }

  char buffer[2048];
  snprintf(buffer, 2048,
  	   "RTSP/1.0 200 OK\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   "Content-Type: application/sdp\r\n"
	   "Cache-Control: no-cache\r\n"
           "Content-Length: %d\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString(), strlen(buffer_sdp)+2);

  AddHeaders(buffer);
  strcat(buffer, buffer_sdp);
  strcat(buffer, "\r\n");

  if(!SendRequest(buffer))
     return FALSE;

  rtsp_state = RTSP_DESCRIBE;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::ParseTransportStr(SipCapability *sc, PString & transport_str)
{
  if(sc == NULL)
  {
    MCUTRACE(1, trace_section << "error");
    return FALSE;
  }

  //RTP/AVP/UDP;unicast;source=192.168.1.1;client_port=5002-5003;server_port=52069-52070;ssrc=C7F3A123;mode=play
  //RTP/AVP;unicast;client_port=55986-55987
  MCUStringDictionary transport_dict(transport_str);
  PString local_ports, remote_ports;

  if(direction == DIRECTION_INBOUND)
  {
    sc->remote_ip = MCUURL(ruri_str).GetHostName();
    remote_ports = transport_dict("client_port");
    sc->remote_port = remote_ports.Tokenise("-")[0].AsInteger();
  } else {
    sc->remote_ip = transport_dict("source");
    if(sc->remote_ip == "")
      sc->remote_ip = listener->GetSocketHost();
    remote_ports = transport_dict("server_port");
    sc->remote_port = remote_ports.Tokenise("-")[0].AsInteger();
  }

  if(sc->remote_ip == "" || sc->remote_ip == "0.0.0.0" || sc->remote_port == 0)
  {
    MCUTRACE(1, trace_section << "missing remote ip or remote port");
    return FALSE;
  }

  if(direction == DIRECTION_INBOUND)
  {
    if(sc->media == MEDIA_TYPE_AUDIO)
    {
      if(audio_rtp_port == 0)
      {
        MCUTRACE(1, trace_section << "error");
        return FALSE;
      }
      local_ports = PString(audio_rtp_port)+"-"+PString(audio_rtp_port+1);
    }
    else if(sc->media == MEDIA_TYPE_VIDEO)
    {
      if(video_rtp_port == 0)
      {
        MCUTRACE(1, trace_section << "error");
        return FALSE;
      }
      local_ports = PString(video_rtp_port)+"-"+PString(video_rtp_port+1);
    }
    transport_dict.Append("source", nat_ip);
    transport_dict.Append("server_port", local_ports);
    transport_str = transport_dict.AsString();
    transport_str.Replace("=;",";",TRUE,0);
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnRequestSetup(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);
  SipCapability *sc = NULL;

  PString setup_media;
  MCUURL url(url_as_string(msg_home(msg), sip->sip_request->rq_url));
  if(url.GetPath().GetSize() == 1)
    setup_media = url.GetPath()[0];
  else if(url.GetPath().GetSize() > 1)
    setup_media = url.GetPath()[1];
  else
  {
    MCUTRACE(1, trace_section << "incorrect path " << url.GetPathStr());
    return FALSE;
  }

  if(setup_media == "audio")
    sc = FindSipCap(RemoteSipCaps, scap);
  else if(setup_media == "video")
    sc = FindSipCap(RemoteSipCaps, vcap);
  else
  {
    MCUTRACE(1, trace_section << "unknown media " << setup_media);
    return FALSE;
  }

  PString transport_str;
  for(sip_unknown_t *sip_un = sip->sip_unknown; sip_un != NULL; sip_un = sip_un->un_next)
  {
    if(PString(sip_un->un_name) == "Transport")
      transport_str = sip_un->un_value;
  }

  if(ParseTransportStr(sc, transport_str) == FALSE)
  {
    MCUTRACE(1, trace_section << "failed parse transport header");
    return FALSE;
  }

  char buffer[1024];
  snprintf(buffer, 1024,
  	   "RTSP/1.0 200 OK\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   "Session: %s\r\n"
	   "Transport: %s\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString(), (const char *)rtsp_session_str, (const char *)transport_str);

  AddHeaders(buffer);
  if(SendRequest(buffer) == 0)
     return FALSE;

  rtsp_state = RTSP_SETUP;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnRequestPlay(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);

  // creating conference if needed
  ConferenceManager *manager = OpenMCU::Current().GetConferenceManager();
  conference = manager->MakeConferenceWithLock(requestedRoom);
  if(conference == NULL)
    return FALSE;

  conferenceMember = new ConferenceStreamMember(conference, GetCallToken(), "RTSP "+rtsp_path+" ("+ruri_str+")");

   // unlock conference
  conference->Unlock();

  // start rtp channels
  CreateMediaChannel(scap, 1);
  CreateMediaChannel(vcap, 1);
  StartTransmitChannels();

  // is connected
  connectionState = EstablishedConnection;

  char buffer[1024];
  snprintf(buffer, 1024,
  	   "RTSP/1.0 200 OK\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   "Session: %s\r\n"
	   "Range: npt=0.000-\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString(), (const char *)rtsp_session_str);

  AddHeaders(buffer);
  if(!SendRequest(buffer))
     return FALSE;

  rtsp_state = RTSP_PLAYING;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnRequestTeardown(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);

  char buffer[1024];
  snprintf(buffer, 1024,
  	   "RTSP/1.0 200 OK\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   "Session: %s\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString(), (const char *)rtsp_session_str);

  AddHeaders(buffer);
  SendRequest(buffer);

  rtsp_state = RTSP_TEARDOWN;

  ClearCall(EndedByRemoteUser);
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspConnection::AddHeaders(char *buffer, PString method_name)
{
  if(direction == DIRECTION_OUTBOUND && auth_type != AUTH_NONE && method_name != METHOD_OPTIONS)
  {
    PString auth_str = sep->MakeAuthStr(auth_username, auth_password, ruri_str, method_name, auth_scheme, auth_realm, auth_nonce);
    strcat(buffer, (const char *)PString("Authorization: "+auth_str+"\r\n"));
  }

  if(direction == DIRECTION_OUTBOUND)
    strcat(buffer, (const char *)PString("User-Agent: "+SIP_USER_AGENT+"\r\n"));
  else
    strcat(buffer, (const char *)PString("Server: "+SIP_USER_AGENT+"\r\n"));

  strcat(buffer, "\r\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::RtspCheckAuth(const msg_t *msg)
{
  if(auth_type == AUTH_NONE)
    return TRUE;

  sip_t *sip = sip_object(msg);
  if(sip->sip_authorization == NULL)
  {
    PString auth_str = auth_scheme+" realm=\""+auth_realm+"\",nonce=\""+auth_nonce+"\",algorithm=MD5";
    char buffer[1024];
    snprintf(buffer, 1024,
  	   "RTSP/1.0 401 Unauthorized\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   "WWW-Authenticate: %s\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString(), (const char *)auth_str);
    AddHeaders(buffer);
    SendRequest(buffer);
    return FALSE;
  }
  else
  {
    PString method_name = sip->sip_request->rq_method_name;
    PString username = msg_params_find(sip->sip_authorization->au_params, "username=");
    PString response = msg_params_find(sip->sip_authorization->au_params, "response=");
    PString uri = msg_params_find(sip->sip_authorization->au_params, "uri=");

    PString auth_str = sep->MakeAuthStr(auth_username, auth_password, uri, method_name, auth_scheme, auth_realm, auth_nonce);
    MCUStringDictionary dict(auth_str, ", ", "=");
    PString auth_response = dict("response");
    if(auth_response != response)
    {
      char buffer[1024];
      snprintf(buffer, 1024,
  	   "RTSP/1.0 403 Forbidden\r\n"
	   "CSeq: %d %s\r\n"
	   "Date: %s\r\n"
	   , sip->sip_cseq->cs_seq, sip->sip_request->rq_method_name, (const char *)PTime().AsString());
      AddHeaders(buffer);
      SendRequest(buffer);

      MCUTRACE(1, trace_section << "authorization failure");
      ClearCall();
      return FALSE;
    }
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::SendRequest(char *buffer)
{
  if(listener->Send(buffer) == FALSE)
  {
    ClearCall();
    return FALSE;
  }

  MCUTRACE(1, trace_section << "send " << strlen(buffer) << " bytes\n" << buffer);
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnResponseReceived(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);
  int response_code = -1;

  int status = sip->sip_status->st_status;

  if(rtsp_state == RTSP_DESCRIBE)
  {
    if(status == 200)
    {
      response_code = OnResponseDescribe(msg);
    }
    else if(status == 401)
    {
      if(auth_type != AUTH_NONE || auth_username == "" || auth_password == "")
      {
        MCUTRACE(1, trace_section << "error");
        return FALSE;
      }
      if(sep->ParseAuthMsg(msg, auth_type, auth_scheme, auth_realm, auth_nonce) == FALSE)
      {
        MCUTRACE(1, trace_section << "error");
        return FALSE;
      }
      SendDescribe();
      return TRUE;
    }
  }
  else if(rtsp_state == RTSP_SETUP_AUDIO || rtsp_state == RTSP_SETUP_VIDEO)
  {
    if(status == 200)
      response_code = OnResponseSetup(msg);
  }
  else if(rtsp_state == RTSP_PLAY)
  {
    if(status == 200)
      response_code = OnResponsePlay(msg);
  }
  else if(rtsp_state == RTSP_TEARDOWN)
  {
    return TRUE;
  }

  if(response_code == -1)
  {
    MCUTRACE(1, trace_section << "unknown response " << status << ", state " << rtsp_state);
    return FALSE;
  }

  if(response_code == FALSE)
  {
    MCUTRACE(1, trace_section << "error processing response " << status << ", error " << response_code << ", state " << rtsp_state);
    return FALSE;
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspConnection::OnRequestReceived(const msg_t *msg)
{
  sip_t *sip = sip_object(msg);
  int response_code = -1;

  PString method_name = sip->sip_request->rq_method_name;

  if(method_name != METHOD_OPTIONS && RtspCheckAuth(msg) == FALSE)
    return FALSE;

  if(method_name == METHOD_OPTIONS)
  {
    response_code = OnRequestOptions(msg);
  }
  else if(method_name == METHOD_DESCRIBE && rtsp_state == RTSP_NONE)
  {
    response_code = OnRequestDescribe(msg);
  }
  else if(method_name == METHOD_SETUP && (rtsp_state == RTSP_DESCRIBE || rtsp_state == RTSP_SETUP))
  {
    response_code = OnRequestSetup(msg);
  }
  else if(method_name == METHOD_PLAY && rtsp_state == RTSP_SETUP)
  {
    response_code = OnRequestPlay(msg);
  }
  else if(method_name == METHOD_TEARDOWN)
  {
    response_code = OnRequestTeardown(msg);
  }

  if(response_code == -1)
  {
    MCUTRACE(1, trace_section << "unknown request " << method_name << ", state " << rtsp_state);
    return FALSE;
  }

  if(response_code == FALSE)
  {
    MCUTRACE(1, trace_section << "error processing request " << method_name << ", error " << response_code << ", state " << rtsp_state);
    return FALSE;
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int MCURtspConnection::OnReceived(MCUSocket *socket, PString data)
{
  PWaitAndSignal m(connMutex);

  msg_t *msg = NULL;
  sip_t *sip = NULL;

  if(socket == NULL)
  {
    MCUTRACE(1, trace_section << "connection closed by remote user");
    goto error;
  }

  MCUTRACE(1, trace_section << "recv from " << socket->GetAddress() << " "  << data.GetLength() << " bytes\n" << data);

  msg = ParseMsg(data);
  if(msg == NULL)
  {
    MCUTRACE(1, trace_section << "failed parse message");
    goto error;
  }

  sip = sip_object(msg);
  if(sip->sip_content_length && sip->sip_content_length->l_length != 0 && sip->sip_payload == NULL)
  {
    MCUTRACE(1, trace_section << "failed parse message, empty payload");
    goto error;
  }

  if(sip->sip_status && !OnResponseReceived(msg))
    goto error;

  if(sip->sip_request && !OnRequestReceived(msg))
    goto error;

  msg_destroy(msg);
  return 1;

  error:
    msg_destroy(msg);
    ClearCall(EndedByRemoteUser);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCURtspServer::MCURtspServer(MCUH323EndPoint *_ep, MCUSipEndPoint *_sep)
  :ep(_ep), sep(_sep)
{
  trace_section = "RTSP server: ";
  StartListeners();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCURtspServer::~MCURtspServer()
{
  RemoveListeners();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspServer::StartListeners()
{
  PWaitAndSignal m(rtspMutex);

  RemoveListeners();

  MCUConfig cfg("RTSP Parameters");
  if(cfg.GetBoolean(EnableKey, TRUE) == FALSE)
    return;

  PStringArray list = cfg.GetString("Listener", "0.0.0.0:1554").Tokenise(",");
  for(PINDEX i = 0; i < list.GetSize(); ++i)
  {
    if(list[i] == "") continue;
    AddListener(list[i]);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspServer::AddListener(PString address)
{
  address.Replace(" ","",TRUE,0);
  if(address.Find("tcp:") == P_MAX_INDEX)
    address = "tcp:"+address;

  MCUURL url(address);
  PString socket_host = url.GetHostName();
  unsigned socket_port = url.GetPort().AsInteger();
  if(socket_host != "0.0.0.0" && PIPSocket::Address(socket_host).IsValid() == FALSE)
  {
    MCUTRACE(1, trace_section << "incorrect listener host " << socket_host);
    return;
  }
  if(socket_host != "0.0.0.0" && PIPSocket::IsLocalHost(socket_host) == FALSE)
  {
    MCUTRACE(1, trace_section << "incorrect listener host " << socket_host << ", this is not a local address");
    return;
  }
  if(socket_port == 0)
  {
    MCUTRACE(1, trace_section << "incorrect listener port " << socket_port);
    return;
  }

  PWaitAndSignal m(rtspMutex);

  MCUListener *listener = MCUListener::Create(MCU_LISTENER_TCP_SERVER, socket_host, socket_port, OnReceived_wrap, this);
  if(listener)
    Listeners.insert(ListenersMapType::value_type(address, listener));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspServer::RemoveListener(PString address)
{
  address.Replace(" ","",TRUE,0);
  if(address.Find("tcp:") == P_MAX_INDEX)
    address = "tcp:"+address;

  PWaitAndSignal m(rtspMutex);

  for(ListenersMapType::iterator it = Listeners.begin(); it != Listeners.end(); ++it)
  {
    if(address == it->first)
    {
      delete it->second;
      Listeners.erase(it);
      break;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspServer::HasListener(PString host, PString port)
{
  PWaitAndSignal m(rtspMutex);

  for(ListenersMapType::iterator it = Listeners.begin(); it != Listeners.end(); ++it)
  {
    if(it->second->GetSocketPort() == port)
    {
      if(it->second->GetSocketHost() == host)
        return TRUE;
      if(it->second->GetSocketHost() == "0.0.0.0" && PIPSocket::IsLocalHost(host))
        return TRUE;
    }
  }
  return FALSE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspServer::RemoveListeners()
{
  PWaitAndSignal m(rtspMutex);

  for(ListenersMapType::iterator it = Listeners.begin(); it != Listeners.end(); )
  {
    delete it->second;
    Listeners.erase(it++);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int MCURtspServer::OnReceived(MCUSocket *socket, PString data)
{
  PWaitAndSignal m(rtspMutex);

  MCUTRACE(1, trace_section << "read from " << socket->GetAddress() << " "  << data.GetLength() << " bytes\n" << data);

  msg_t *msg = ParseMsg(data);
  if(CreateConnection(socket, msg) == FALSE)
  {
    msg_destroy(msg);
    return 0;
  }

  msg_destroy(msg);
  return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCURtspServer::CreateConnection(MCUSocket *socket, const msg_t *msg)
{
  PWaitAndSignal m(rtspMutex);

  if(ep->HasConnection(socket->GetAddress()))
  {
    MCUTRACE(1, trace_section << socket->GetAddress() << " connection already exists ");
    SendResponse(socket, msg, "454 Session Not Found");
    return FALSE;
  }

  if(msg == NULL)
  {
    MCUTRACE(1, trace_section << socket->GetAddress() << " failed parse message ");
    SendResponse(socket, msg, "400 Bad Request");
    return FALSE;
  }

  sip_t *sip = sip_object(msg);
  if(sip->sip_request == NULL || sip->sip_cseq == NULL)
  {
    MCUTRACE(1, trace_section << socket->GetAddress() << " missing headers ");
    SendResponse(socket, msg, "400 Bad Request");
    return FALSE;
  }

  PString agent;
  if(sip->sip_user_agent)
    agent = sip->sip_user_agent->g_string;
  if(agent.Find("RealMedia") != P_MAX_INDEX)
  {
    MCUTRACE(1, trace_section << socket->GetAddress() << " RealRTSP is not supported ");
    SendResponse(socket, msg, "505 RTSP Version not supported");
    return FALSE;
  }

  PString method_name = sip->sip_request->rq_method_name;
  if(method_name != METHOD_OPTIONS && method_name != METHOD_DESCRIBE)
  {
    MCUTRACE(1, trace_section << socket->GetAddress() << " incorrect method " << method_name);
    SendResponse(socket, msg, "455 Method Not Valid in This State");
    return FALSE;
  }

  PString luri_str = url_as_string(msg_home(msg), sip->sip_request->rq_url);
  MCUURL lurl(luri_str);
  PString path = lurl.GetPath()[0];
  if(path == "" || MCUConfig("RTSP Server "+path).GetBoolean(EnableKey) == FALSE)
  {
    MCUTRACE(1, trace_section << socket->GetAddress() << " server not found " << path);
    SendResponse(socket, msg, "404 Not Found");
    return FALSE;
  }

  PString callToken = socket->GetAddress();
  MCURtspConnection *conn = new MCURtspConnection(ep, callToken);
  if(!conn->Connect(socket, msg))
    return FALSE;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool MCURtspServer::CreateConnection(const PString & room, const PString & address, const PString & callToken)
{
  MCURtspConnection *conn = new MCURtspConnection(ep, callToken);
  if(!conn->Connect(room, address))
    return FALSE;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCURtspServer::SendResponse(MCUSocket *socket, const msg_t *msg, const PString & status_str)
{
  char buffer[1024];
  snprintf(buffer, 1024,
  	   "RTSP/1.0 %s\r\n"
	   "Date: %s\r\n"
	   "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\r\n"
	   "\r\n"
	   , (const char *)status_str, (const char *)PTime().AsString());

  socket->SendData(buffer);
  MCUTRACE(1, trace_section << "send to " << socket->GetAddress() << " " << strlen(buffer) << " bytes\n" << buffer);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
