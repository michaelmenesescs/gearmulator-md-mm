#include "mdRemotePanel.h"
#include "mdEditor.h"
#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/rmlInterfaces.h"
#include "juceRmlUi/rmlElemKnob.h"
#include "mdLib/mdpanel.h"
#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/ElementDocument.h"
#include <chrono>
#include <sys/socket.h>
#include "juceRmlUi/rmlElemButton.h"
#include <cmath>
#include <cstring>

namespace mdJucePlugin {
using namespace remotePanel;
namespace {
uint64_t clockUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void u32(std::vector<uint8_t>& b, uint32_t v) { for(int i=0;i<4;++i) b.push_back(uint8_t(v>>(8*i))); }
void u64(std::vector<uint8_t>& b, uint64_t v) { u32(b,uint32_t(v)); u32(b,uint32_t(v>>32)); }
void f32(std::vector<uint8_t>& b, float v) { uint32_t u; std::memcpy(&u,&v,4); u32(b,u); }
}

void RemotePanel::attachEditor(Editor* editor) {
 jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
 if(m_editor) detachEditor(m_editor);
 m_editor=editor;
 startTimer(20);
}
void RemotePanel::detachEditor(Editor* editor) {
 if(m_editor!=editor) return;
 sourceUnavailable(1);
 clearContacts();
 m_editor=nullptr;
 stopTimer();
}
void RemotePanel::sourceUnavailable(uint8_t reason) {
 bool changed=false;
 std::unique_lock sourceLock(m_panelMutex);
 {
  changed=m_sourceAvailable.exchange(false) || m_sourceReason.load()!=reason;
  if(!changed) return;
  ++m_geometryGeneration;
  m_sourceReason=reason; m_controlsRequested=true;
  m_panelImage.reset(); m_pendingCapture={};
 }
 std::vector<uint32_t> clients;
 { std::lock_guard lock(m_clientsMutex); for(auto& c:m_clients) clients.push_back(c.first); }
 for(auto id:clients) releaseClient(id);
 { std::lock_guard lock(m_encoderMutex); m_encoderPending.fill(0); }
 sourceLock.unlock();
 m_server.notifyFrame();
 log("source unavailable reason="+std::to_string(reason)+" generation="+std::to_string(m_geometryGeneration.load()));
}
void RemotePanel::sendSource(const ClientPtr&) {
 // Source state and panel chunks share one writer ordering. Queueing a separate
 // source message here could race recovery and leave a client permanently blank.
 m_server.notifyFrame();
}
void RemotePanel::timerCallback() {
 jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
 if(!m_editor) return;
 auto* component=m_editor->getRmlComponent();
 if(!component || !component->getContext() || !component->isShowing()
  || (component->getPeer() && component->getPeer()->isMinimised())) { sourceUnavailable(2); clearContacts(); return; }
 juceRmlUi::RmlInterfaces::ScopedAccess access(*component);
 auto* context=component->getContext();
 const auto dims=context->GetDimensions();
 const auto scale=context->GetDensityIndependentPixelRatio();
 auto* doc=component->getDocument();
 if(!doc || scale<=0 || dims.x<=0 || dims.y<=0) return;
 const auto origin=doc->GetAbsoluteOffset(Rml::BoxArea::Border);
 const auto size=doc->GetBox().GetSize(Rml::BoxArea::Border);
 Geometry geometry{origin.x,origin.y,size.x/scale,size.y/scale,scale,uint32_t(dims.x),uint32_t(dims.y)};
 if(!(geometry==m_geometry)) {
  sourceUnavailable(4); clearContacts();
  std::lock_guard lock(m_panelMutex); m_geometry=geometry; m_controlsRequested=true;
 }
 if(m_controlsRequested.exchange(false)) {
  juce::Array<juce::var> controls;
  std::function<void(Rml::Element*)> visit=[&](Rml::Element* element) {
   if(element->HasAttribute("remote-slot")) {
    auto* value=new juce::DynamicObject;
    const auto p=element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto b=element->GetBox().GetSize(Rml::BoxArea::Border);
    value->setProperty("slot",element->GetAttribute<int>("remote-slot",-1));
    value->setProperty("id",juce::String(element->GetId()));
    value->setProperty("x",(p.x-origin.x)/scale); value->setProperty("y",(p.y-origin.y)/scale);
    value->setProperty("w",b.x/scale); value->setProperty("h",b.y/scale);
    controls.add(juce::var(value));
   }
   for(int i=0;i<element->GetNumChildren();++i) visit(element->GetChild(i));
  };
  visit(doc);
  auto* value=new juce::DynamicObject; value->setProperty("t","panelControls");
  value->setProperty("generation",int(m_geometryGeneration.load())); value->setProperty("controls",controls);
  const auto json=juce::JSON::toString(juce::var(value),true).toStdString();
  m_server.sendToAll(std::vector<uint8_t>(json.begin(),json.end()),true);
 }
 const auto now=clockUs();
 { std::lock_guard panelLock(m_panelMutex);
 for(auto& entry:m_contacts) {
  auto& c=entry.second;
  if(!m_sourceAvailable || c.generation!=m_geometryGeneration) continue;
  if(c.slot>=g_buttonSlots && c.holdEligible && !c.held && now-c.downUs>=400000) {
   c.held=true;
   handleHold(entry.first.first,c.slot,true,entry.first.second);
   updateContactVisual(c.element.get());
  }
  updateContactVisual(c.element.get());
 }
 }
 if(now-m_lastCaptureRequestUs<66667 || m_panelClients.load()==0) return;
 m_lastCaptureRequestUs=now;
 ++m_captureRequested;
 const auto generation=m_geometryGeneration.load();
 const auto led=m_editor->m_remoteLedSequence;
 std::weak_ptr<int> alive=m_captureLifetime;
 if(component->takeScreenshot([this,alive,geometry,generation,led](const juce::Image& image) {
  if(alive.expired()) return;
  // Bounded handoff only: no compression, sockets, recursion, or device lock.
  if(image.isNull() || image.getWidth()>2560 || image.getHeight()>1600
   || image.getWidth()!=int(geometry.contextWidth) || image.getHeight()!=int(geometry.contextHeight)) return;
  Capture c; c.image=image.createCopy(); c.geometry=geometry; c.generation=generation;
  c.sequence=++m_captureCompleted; c.completedUs=clockUs(); c.ledSequence=led;
  std::lock_guard lock(m_panelMutex);
  if(generation!=m_geometryGeneration) return;
  m_lastCaptureUs=c.completedUs;
  m_pendingCapture=std::move(c);
 })) ++m_captureAccepted;
}
void RemotePanel::servicePanelCapture() {
 Capture c;
 { std::lock_guard lock(m_panelMutex); std::swap(c,m_pendingCapture); }
 if(c.image.isNull()) return;
 // Only the existing composed image is scaled. No re-rendering of skin or LCD.
 if(c.image.getWidth()>1100) c.image=c.image.rescaled(1100,std::max(1,c.image.getHeight()*1100/c.image.getWidth()),juce::Graphics::mediumResamplingQuality);
 juce::MemoryOutputStream out;
 if(!juce::PNGImageFormat().writeImageToStream(c.image,out) || out.getDataSize()>2*1024*1024) { sourceUnavailable(5); return; }
 auto frame=std::make_shared<PanelImage>();
 const auto* bytes=static_cast<const uint8_t*>(out.getData());
 frame->png.assign(bytes,bytes+out.getDataSize());
 frame->generation=c.generation; frame->sequence=c.sequence; frame->completedUs=c.completedUs; frame->ledSequence=c.ledSequence;
 frame->width=c.image.getWidth(); frame->height=c.image.getHeight();
 auto& s=frame->source; s={uint8_t(Msg::PanelSource),1,0}; u32(s,c.generation);
 f32(s,c.geometry.originX); f32(s,c.geometry.originY); f32(s,c.geometry.width); f32(s,c.geometry.height); f32(s,c.geometry.scale);
 u32(s,c.geometry.contextWidth); u32(s,c.geometry.contextHeight); u32(s,frame->width); u32(s,frame->height);
 bool restored=false;
 {
  std::lock_guard lock(m_panelMutex);
  if(c.generation!=m_geometryGeneration || clockUs()-c.completedUs>750000) return;
  restored=!m_sourceAvailable;
  if(!restored && frame->png==m_lastPng) return;
  m_lastPng=frame->png;
  m_panelImage=frame; m_sourceReason=0; m_sourceAvailable=true;
 }
 if(restored) log("source available generation="+std::to_string(c.generation)+" raster="+std::to_string(frame->width)+"x"+std::to_string(frame->height));
 m_server.notifyFrame();
}
std::vector<uint8_t> RemotePanel::buildPanelFrame(Server::Client& client) {
 if(!client.panelSubscribed) return {};
 std::shared_ptr<const PanelImage> latest;
 uint32_t generation;
 uint8_t reason;
 { std::lock_guard lock(m_panelMutex);
  generation=m_geometryGeneration; reason=m_sourceReason;
  if(m_sourceAvailable) latest=m_panelImage;
 }
 if(!latest) {
  client.panelSending.reset(); client.panelReadyGeneration=0;
  if(client.panelSourceGeneration==generation && !client.panelSourceAvailable) return {};
  client.panelSourceGeneration=generation; client.panelSourceAvailable=false;
  std::vector<uint8_t> state{uint8_t(Msg::PanelSource),0,reason}; u32(state,generation); state.resize(43,0);
  return state;
 }
 if(client.panelSourceGeneration!=latest->generation || !client.panelSourceAvailable) {
  client.panelSending.reset(); client.panelSentSequence=0; client.panelReadyGeneration=0;
  client.panelSourceGeneration=latest->generation; client.panelSourceAvailable=true;
  return latest->source;
 }
 if(!client.panelSending) {
  if(client.panelSentSequence==latest->sequence) return {};
  client.panelSending=latest; client.panelOffset=0;
 }
 const auto& f=*client.panelSending;
 const auto count=std::min<size_t>(16384,f.png.size()-client.panelOffset);
 if(!client.panelBudget.consume(clockUs(),count+41)) return {};
 std::vector<uint8_t> msg{uint8_t(Msg::PanelChunk)};
 u32(msg,f.generation); u64(msg,f.sequence); u64(msg,f.ledSequence); u64(msg,f.completedUs);
 u32(msg,uint32_t(f.png.size())); u32(msg,uint32_t(client.panelOffset)); u32(msg,uint32_t(count));
 msg.insert(msg.end(),f.png.begin()+client.panelOffset,f.png.begin()+client.panelOffset+count);
 client.panelOffset+=count; m_panelBytes+=msg.size();
 if(client.panelOffset==f.png.size()) {
  client.panelSentSequence=f.sequence; client.panelDeliveredSequence=f.sequence; client.panelDeliveredGeneration=f.generation;
  client.panelSending.reset(); ++m_panelFrames;
 }
 return msg;
}
void RemotePanel::queueTouch(const ClientPtr& client,const uint8_t* data,size_t size,uint64_t received) {
 ++m_touchReceived;
 TouchEvent e; e.client=client; e.receivedUs=received;
 const auto decoded=decodeTouch(data,size);
 e.phase=decoded.phase; e.sequence=decoded.sequence; e.contact=decoded.contact;
 e.generation=decoded.generation; e.x=decoded.x; e.y=decoded.y;
 { std::lock_guard lock(m_touchMutex);
  if(m_touchEvents.size()>=1024) { client->open=false; ::shutdown(client->fd,SHUT_RDWR); return; }
  m_touchEvents.push_back(e);
 }
 triggerAsyncUpdate();
}
void RemotePanel::updateContactVisual(Rml::Element* element) {
 if(!element) return;
 bool held=false,hover=false;
 for(const auto& e:m_contacts) if(e.second.element.get()==element) { hover=true; held|=e.second.held; }
 // Leave ElemButton's own checked member under desktop ownership. Remote
 // presentation is a pseudo-class overlay and must not suppress a local down.
 bool local=false;
 if(const auto* button=dynamic_cast<juceRmlUi::ElemButton*>(element)) local=button->isChecked();
 bool changed=false;
 const auto set=[&](const char* name,bool value) {
  if(element->IsPseudoClassSet(name)!=value) { element->SetPseudoClass(name,value); changed=true; }
 };
 set("active",held||local); set("hover",hover); set("checked",held||local);
 if(changed && m_editor) if(auto* r=m_editor->getRmlComponent()) r->enqueueUpdateOnce();
}
void RemotePanel::clearContacts(uint32_t client) {
 std::unique_ptr<juceRmlUi::RmlInterfaces::ScopedAccess> access;
 if(m_editor) if(auto* component=m_editor->getRmlComponent())
  access=std::make_unique<juceRmlUi::RmlInterfaces::ScopedAccess>(*component);
 for(auto i=m_contacts.begin();i!=m_contacts.end();) {
  if(client && i->first.first!=client) { ++i; continue; }
  auto element=i->second.element;
  if(i->second.held) handleHold(i->first.first,i->second.slot,false,i->first.second);
  i=m_contacts.erase(i); updateContactVisual(element.get());
 }
}
void RemotePanel::handleAsyncUpdate() {
 jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
 std::deque<TouchEvent> events;
 { std::lock_guard lock(m_touchMutex); events.swap(m_touchEvents); }
 for(auto& e:events) {
  auto* component=m_editor ? m_editor->getRmlComponent() : nullptr;
  // All element access is confined to this owner, under the same interface lock as update/render.
  std::unique_ptr<juceRmlUi::RmlInterfaces::ScopedAccess> access;
  if(component) access=std::make_unique<juceRmlUi::RmlInterfaces::ScopedAccess>(*component);
  if(e.phase==4) { clearContacts(e.generation); continue; }
  if(!e.client || !e.client->open) continue;
  std::lock_guard panelLock(m_panelMutex);
  AckStatus status=AckStatus::Accepted; int slot=-1,detents=0; uint64_t resolved=clockUs();
  const auto key=std::make_pair(e.client->id,e.contact);
  auto it=m_contacts.find(key);
  bool cancelled=false;
  { std::lock_guard lock(m_clientsMutex);
   const auto client=m_clients.find(e.client->id);
   cancelled=client==m_clients.end() || e.receivedUs<=client->second.lastReleaseUs;
  }
  if(cancelled) status=AckStatus::Ignored;
  else if(e.phase==255) status=AckStatus::Malformed;
  else if(!component || !m_sourceAvailable || clockUs()-m_lastCaptureUs.load()>750000) status=AckStatus::Unavailable;
  else if(e.generation!=m_geometryGeneration) status=AckStatus::StaleGeometry;
  else if(e.client->panelReadyGeneration!=e.generation) status=AckStatus::Unavailable;
  else if(e.phase==0) {
   if(it!=m_contacts.end()) status=AckStatus::Ignored;
   else if(m_contacts.size()>=256) status=AckStatus::Rejected;
   else if(e.x<0 || e.y<0 || e.x>=m_geometry.width || e.y>=m_geometry.height) status=AckStatus::Miss;
   else {
    auto* hit=component->getContext()->GetElementAtPoint({m_geometry.originX+e.x*m_geometry.scale,m_geometry.originY+e.y*m_geometry.scale});
    while(hit && !hit->HasAttribute("remote-slot")) hit=hit->GetParentNode();
    resolved=clockUs();
    if(!hit) status=AckStatus::Miss;
    else {
     slot=hit->GetAttribute<int>("remote-slot",-1);
     Contact c; c.element=hit->GetObserverPtr(hit->GetCoreInstance()); c.slot=uint8_t(slot); c.startX=c.x=e.x; c.startY=c.y=e.y;
     c.downUs=resolved; c.generation=e.generation; c.held=slot<64;
     for(const auto& other:m_contacts) if(other.second.element.get()==hit) {
      c.drives=false;
      if(slot>=64) { c.held=other.second.held; c.holdEligible=other.second.holdEligible; c.downUs=other.second.downUs; }
      break;
     }
     m_contacts[key]=c;
     if(c.held) status=handleHold(e.client->id,c.slot,true,e.contact);
     updateContactVisual(hit);
    }
   }
  } else if(it==m_contacts.end()) status=AckStatus::Ignored;
  else {
   auto& c=it->second; slot=c.slot; auto element=c.element;
   if(e.phase==1) {
    if(c.drives && (std::abs(e.x-c.startX)+std::abs(e.y-c.startY))*m_geometry.scale>10)
     for(auto& other:m_contacts) if(other.second.element.get()==element.get()) other.second.holdEligible=false;
    if(c.slot>=64 && c.drives) if(auto* knob=dynamic_cast<juceRmlUi::ElemKnob*>(element.get())) {
     const auto delta=knob->remoteDragValueDelta((e.x-c.x)*m_geometry.scale,(e.y-c.y)*m_geometry.scale);
     c.fraction+=delta;
     // Update the real sprite without dispatching Change (which would double ingress).
     auto value=std::fmod(knob->getValue()+delta-knob->getMinValue(),knob->getRange());
     if(value<0) value+=knob->getRange();
     knob->setValue(knob->getMinValue()+value,false);
     if(c.slot<72) m_editor->m_encLast[c.slot-64]=knob->getValue();
     else if(c.slot==72) m_editor->m_levelLast=knob->getValue();
     else if(c.slot==73) m_editor->m_soundLast=knob->getValue();
     component->enqueueUpdateOnce();
     detents=int(c.fraction); c.fraction-=detents;
     if(detents) status=handleEncoderDelta(c.slot-64,detents);
    }
    c.x=e.x; c.y=e.y;
   } else {
    if(c.held) status=handleHold(e.client->id,c.slot,false,e.contact);
    m_contacts.erase(it); updateContactVisual(element.get());
   }
  }
  const auto enqueued=clockUs();
  uint32_t owners=0;
  { std::lock_guard lock(m_rowMutex); if(slot>=0) owners=m_rowOwners.count(uint8_t(slot)); }
  std::vector<uint8_t> ack{uint8_t(Msg::TouchAck)}; u32(ack,e.sequence); ack.push_back(uint8_t(status)); ack.push_back(uint8_t(slot));
  u32(ack,m_geometryGeneration); u64(ack,e.receivedUs); u64(ack,resolved); u64(ack,enqueued); u32(ack,owners); u32(ack,uint32_t(detents));
  m_server.send(e.client,std::move(ack));
 }
}
}
