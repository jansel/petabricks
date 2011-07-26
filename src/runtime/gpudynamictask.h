/*****************************************************************************
 *  Copyright (C) 2008-2011 Massachusetts Institute of Technology            *
 *                                                                           *
 *  Permission is hereby granted, free of charge, to any person obtaining    *
 *  a copy of this software and associated documentation files (the          *
 *  "Software"), to deal in the Software without restriction, including      *
 *  without limitation the rights to use, copy, modify, merge, publish,      *
 *  distribute, sublicense, and/or sell copies of the Software, and to       *
 *  permit persons to whom the Software is furnished to do so, subject       *
 *  to the following conditions:                                             *
 *                                                                           *
 *  The above copyright notice and this permission notice shall be included  *
 *  in all copies or substantial portions of the Software.                   *
 *                                                                           *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY                *
 *  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE               *
 *  WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND      *
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE   *
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION   *
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION    *
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE           *
 *                                                                           *
 *  This source code is part of the PetaBricks project:                      *
 *    http://projects.csail.mit.edu/petabricks/                              *
 *                                                                           *
 *****************************************************************************/
#ifdef HAVE_OPENCL

#ifndef PETABRICKSGPUDYNAMICTASK_H
#define PETABRICKSGPUDYNAMICTASK_H

#include "dynamictask.h"
#include "gputaskinfo.h"

#include <list>

namespace petabricks {

class GpuDynamicTask;
typedef jalib::JRef<GpuDynamicTask> GpuDynamicTaskPtr;
typedef std::vector<GpuDynamicTaskPtr> GpuDynamicTaskList;

class GpuDynamicTask : public DynamicTask {
public:

  virtual DynamicTaskPtr run() = 0;

  enum GpuTaskType {
    PREPARE,
    COPYIN,
    RUN,
    COPYOUT,
    GEN,
  };

  GpuTaskType tasktype() { return _tasktype; }
  GpuTaskInfoPtr taskinfo() { return _taskinfo; }
  MatrixStorageInfoPtr storageinfo() { return _storageinfo; }

  GpuDynamicTask(GpuTaskInfoPtr taskinfo, GpuTaskType tasktype = GEN, MatrixStorageInfoPtr info = NULL);

protected:
  void remoteScheduleTask();
  GpuTaskType _tasktype;
  GpuTaskInfoPtr _taskinfo;
  MatrixStorageInfoPtr _storageinfo;
};

}

#endif
#endif
