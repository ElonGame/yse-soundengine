/*
  ==============================================================================

    channelImplementation.cpp
    Created: 30 Jan 2014 4:21:26pm
    Author:  yvan

  ==============================================================================
*/

#include "channelImplementation.h"
#include "soundImplementation.h"
#include "../utils/misc.hpp"
#include "../internal/global.h"
#include "../internal/underWaterEffect.h"
#include "../internal/reverbManager.h"
#include "../internal/channelManager.h"
#include "../internal/deviceManager.h"

YSE::INTERNAL::channelImplementation& YSE::INTERNAL::channelImplementation::volume(Flt value) {
  newVolume = value;
  Clamp(newVolume, 0.f, 1.f);
  return (*this);
}

Flt YSE::INTERNAL::channelImplementation::volume() {
  return newVolume;
}

YSE::INTERNAL::channelImplementation::channelImplementation(const String & name, channel * head) : 
    ThreadPoolJob(name),
    newVolume(1.f), lastVolume(1.f), userChannel(true),
    allowVirtual(true), objectStatus(CIS_CONSTRUCTED), parent(NULL),
    head(head) {
}

void YSE::INTERNAL::channelImplementation::setup() {
  if (objectStatus >= CIS_CREATED) {
    out.resize(INTERNAL::Global.getChannelManager().getNumberOfOutputs());
    outConf.resize(INTERNAL::Global.getChannelManager().getNumberOfOutputs());
    for (UInt i = 0; i < INTERNAL::Global.getChannelManager().getNumberOfOutputs(); i++) {
      outConf[i].angle = INTERNAL::Global.getChannelManager().getOutputAngle(i);
    }
    objectStatus = CIS_SETUP;
  }
}

Bool YSE::INTERNAL::channelImplementation::readyCheck() {
  if (objectStatus == CIS_SETUP) {
    // make sure the number of is not changed since setup
    if (outConf.size() == Global.getChannelManager().getNumberOfOutputs()) {
      objectStatus = CIS_READY;
      return true;
    }
    else {
      // setup has changed, return to loading stage
      objectStatus = CIS_CREATED;
    }
  }
  return false;
}

YSE::INTERNAL::channelImplementation::~channelImplementation() {
  exit(); // exit the dsp thread for this channel
}

void YSE::INTERNAL::channelImplementation::sync() {
  // remove if interface is gone
  if (head == NULL) {
    objectStatus = CIS_RELEASE;
  }

  // get new values from head
  if (head->flagVolume) {
    newVolume = head->volume;
    head->flagVolume = false;
  }

  if (head->moveChannel) {
    head->newChannel.load()->pimpl->add(this);
    head->moveChannel = false;
  }
}


void YSE::INTERNAL::channelImplementation::attachUnderWaterFX() {
  UnderWaterEffect().channel(this);
}

//TODO: add and remove functions are unlikely to be threadsafe!
Bool YSE::INTERNAL::channelImplementation::add(YSE::INTERNAL::channelImplementation * ch) {
  if (ch != this) {
    if (ch->parent != NULL) ch->parent->remove(ch);
    ch->parent = this;
    children.push_front(ch);
    return true;
  }
  else ch->parent = NULL;
  return false;
}

Bool YSE::INTERNAL::channelImplementation::remove(YSE::INTERNAL::channelImplementation *ch) {
  children.remove(ch);
  return true;
}

Bool YSE::INTERNAL::channelImplementation::add(YSE::INTERNAL::soundImplementation * s) {
  if (s->parent != NULL) {
    s->parent->remove(s);
  }
  s->parent = this;
  sounds.push_front(s);
  return true;
}

Bool YSE::INTERNAL::channelImplementation::remove(YSE::INTERNAL::soundImplementation*s) {
  sounds.remove(s);
  return true;
}


void YSE::INTERNAL::channelImplementation::clearBuffers() {
  for (UInt i = 0; i < out.size(); ++i) {
    out[i] = 0.0f;
  }
}

ThreadPoolJob::JobStatus YSE::INTERNAL::channelImplementation::runJob() {
    dsp();
    return jobHasFinished;
}

void YSE::INTERNAL::channelImplementation::exit() {
  Global.waitForFastJob(this);
}

void YSE::INTERNAL::channelImplementation::dsp() {
  // if no sounds or other channels are linked, we skip this channel
  if (children.empty() && sounds.empty()) return;

  // claim dsp critical section to block parent channel later on
  const ScopedLock lock(dspActive);

  // clear channel buffer
  clearBuffers();

  // calculate child channels if there are any
  for (auto i = children.begin(); i != children.end(); ++i) {
    Global.addFastJob(*i);
  }

  // calculate sounds in this channel
  for (auto i = sounds.begin(); i != sounds.end(); ++i) {
    if ((*i)->dsp()) {
      (*i)->toChannels();
    }
  }

  adjustVolume();

  Global.getReverbManager().process(this);
  
  if (UnderWaterEffect().channel() == this) {
    UnderWaterEffect().apply(out);
  }
  
}

void YSE::INTERNAL::channelImplementation::adjustVolume() {
  Flt volume = newVolume;
  if (volume != lastVolume) {
    // new value, create a ramp
    Flt step = (volume - lastVolume) / STANDARD_BUFFERSIZE;

    for (UInt i = 0; i < out.size(); ++i) {
      Flt multiplier = lastVolume;
      Flt * ptr = out[i].getBuffer();
      for (UInt j = 0; j < STANDARD_BUFFERSIZE; j++) {
        *ptr++ *= multiplier;
        multiplier += step;
      }
    }
    lastVolume = volume;
  }
  else {
    // same volume, just copy
    for (UInt i = 0; i < out.size(); ++i) {
      out[i] *= volume;
    }
  }
}

void YSE::INTERNAL::channelImplementation::buffersToParent() {
  Global.waitForFastJob(this);

  // call this recursively on all child channels 
  for (auto i = children.begin(); i != children.end(); ++i) {
    (*i)->buffersToParent();
  }


  // if this is the main channel, we're done here
  if (parent == NULL) return;
  if (children.empty() && sounds.empty()) return;

  // if not the main channel, add output to parent channel
  for (UInt i = 0; i < out.size(); ++i) {
    // parent size is not checked but should be ok because it's adjusted before calling this
    parent->out[i] += out[i];
  }
}

YSE::INTERNAL::channelImplementation& YSE::INTERNAL::channelImplementation::allowVirtualSounds(Bool value) {
  allowVirtual = value;
  for (auto i = children.begin(); i != children.end(); ++i) {
    (*i)->allowVirtualSounds(value);
  }
  return (*this);
}

Bool YSE::INTERNAL::channelImplementation::allowVirtualSounds() {
  return allowVirtual;
}
