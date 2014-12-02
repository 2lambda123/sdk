/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (http://www.swig.org).
 * Version 2.0.10
 *
 * Do not make changes to this file unless you know what you are doing--modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

package com.mega.sdk;

class MegaRequestListener {
  private long swigCPtr;
  protected boolean swigCMemOwn;

  protected MegaRequestListener(long cPtr, boolean cMemoryOwn) {
    swigCMemOwn = cMemoryOwn;
    swigCPtr = cPtr;
  }

  protected static long getCPtr(MegaRequestListener obj) {
    return (obj == null) ? 0 : obj.swigCPtr;
  }

  protected void finalize() {
    delete();
  }

  protected synchronized void delete() {   
    if (swigCPtr != 0) {
      if (swigCMemOwn) {
        swigCMemOwn = false;
        megaJNI.delete_MegaRequestListener(swigCPtr);
      }
      swigCPtr = 0;
    }
}

  protected void swigDirectorDisconnect() {
    swigCMemOwn = false;
    delete();
  }

  public void swigReleaseOwnership() {
    swigCMemOwn = false;
    megaJNI.MegaRequestListener_change_ownership(this, swigCPtr, false);
  }

  public void swigTakeOwnership() {
    swigCMemOwn = true;
    megaJNI.MegaRequestListener_change_ownership(this, swigCPtr, true);
  }

  public void onRequestStart(MegaApi api, MegaRequest request) {
    if (getClass() == MegaRequestListener.class) megaJNI.MegaRequestListener_onRequestStart(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request); else megaJNI.MegaRequestListener_onRequestStartSwigExplicitMegaRequestListener(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request);
  }

  public void onRequestFinish(MegaApi api, MegaRequest request, MegaError e) {
    if (getClass() == MegaRequestListener.class) megaJNI.MegaRequestListener_onRequestFinish(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request, MegaError.getCPtr(e), e); else megaJNI.MegaRequestListener_onRequestFinishSwigExplicitMegaRequestListener(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request, MegaError.getCPtr(e), e);
  }

  public void onRequestUpdate(MegaApi api, MegaRequest request) {
    if (getClass() == MegaRequestListener.class) megaJNI.MegaRequestListener_onRequestUpdate(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request); else megaJNI.MegaRequestListener_onRequestUpdateSwigExplicitMegaRequestListener(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request);
  }

  public void onRequestTemporaryError(MegaApi api, MegaRequest request, MegaError error) {
    if (getClass() == MegaRequestListener.class) megaJNI.MegaRequestListener_onRequestTemporaryError(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request, MegaError.getCPtr(error), error); else megaJNI.MegaRequestListener_onRequestTemporaryErrorSwigExplicitMegaRequestListener(swigCPtr, this, MegaApi.getCPtr(api), api, MegaRequest.getCPtr(request), request, MegaError.getCPtr(error), error);
  }

  public MegaRequestListener() {
    this(megaJNI.new_MegaRequestListener(), true);
    megaJNI.MegaRequestListener_director_connect(this, swigCPtr, swigCMemOwn, true);
  }

}
