/*============================================================================
  CMake - Cross Platform Makefile Generator
  Copyright 2015 Stephen Kelly <steveire@gmail.com>

  Distributed under the OSI-approved BSD License (the "License");
  see accompanying file Copyright.txt for details.

  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the License for more information.
============================================================================*/

#include "cmServer.h"

#include "cmVersionMacros.h"
#include "cmGlobalGenerator.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  *buf = uv_buf_init((char*) malloc(suggested_size), suggested_size);
}

void free_write_req(uv_write_t *req) {
  write_req_t *wr = (write_req_t*) req;
  free(wr->buf.base);
  free(wr);
}

void on_stdout_write(uv_write_t *req, int status) {
  (void)status;
  auto server = reinterpret_cast<cmMetadataServer*>(req->data);
  free_write_req(req);
  server->PopOne();
}

void write_data(uv_stream_t *dest, std::string content, uv_write_cb cb) {
  write_req_t *req = (write_req_t*) malloc(sizeof(write_req_t));
  req->req.data = dest->data;
  req->buf = uv_buf_init((char*) malloc(content.size()), content.size());
  memcpy(req->buf.base, content.c_str(), content.size());
  uv_write((uv_write_t*) req, (uv_stream_t*)dest, &req->buf, 1, cb);
}

void read_stdin(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  if (nread > 0) {
    auto server = reinterpret_cast<cmMetadataServer*>(stream->data);
    std::string result = std::string(buf->base, buf->base + nread);
    server->handleData(result);
  }

  if (buf->base)
    free(buf->base);
}

cmMetadataServer::cmMetadataServer()
  : CMakeInstance(0)
{
  mLoop = uv_default_loop();

  uv_pipe_init(mLoop, &mStdin_pipe, 0);
  uv_pipe_open(&mStdin_pipe, 0);
  mStdin_pipe.data = this;

  uv_pipe_init(mLoop, &mStdout_pipe, 0);
  uv_pipe_open(&mStdout_pipe, 1);
  mStdout_pipe.data = this;

  this->State = Uninitialized;
  this->Writing = false;
}

cmMetadataServer::~cmMetadataServer() {
  uv_close((uv_handle_t *)&mStdin_pipe, NULL);
  uv_close((uv_handle_t *)&mStdout_pipe, NULL);
  uv_loop_close(mLoop);
  delete this->CMakeInstance;
}

void cmMetadataServer::PopOne()
{
  this->Writing = false;
  if (mQueue.empty())
    {
    return;
    }
  this->processRequest(mQueue.front());
  mQueue.erase(mQueue.begin());
}

void cmMetadataServer::handleData(const std::string &data)
{
  mDataBuffer += data;

  for ( ; ; )
    {
    auto needle = mDataBuffer.find('\n');

    if (needle == std::string::npos)
      {
      return;
      }
    std::string line = mDataBuffer.substr(0, needle);
    mDataBuffer.erase(mDataBuffer.begin(), mDataBuffer.begin() + needle + 1);
    if (line == "[== CMake MetaMagic ==[")
      {
      mJsonData.clear();
      continue;
      }
    mJsonData += line;
    mJsonData += "\n";
    if (line == "]== CMake MetaMagic ==]")
      {
      mQueue.push_back(mJsonData);
      mJsonData.clear();
      if (!this->Writing)
        {
        this->PopOne();
        }
      }
    }
}

void cmMetadataServer::ServeMetadata(const std::string& buildDir)
{
  m_buildDir = buildDir;

  this->State = Started;

  Json::Value obj = Json::objectValue;
  obj["progress"] = "process-started";
  this->WriteResponse(obj);

  uv_read_start((uv_stream_t*)&mStdin_pipe, alloc_buffer, read_stdin);

  uv_run(mLoop, UV_RUN_DEFAULT);
}

void cmMetadataServer::processRequest(const std::string& json)
{
  Json::Reader reader;
  Json::Value value;
  reader.parse(json, value);

  if (this->State == Started)
    {
    if (value["type"] == "handshake")
      {
      this->ProcessHandshake(value["protocolVersion"].asString());
      }
    }
  if (this->State == ProcessingRequests)
    {
    if (value["type"] == "version")
      {
      this->ProcessVersion();
      }
    if (value["type"] == "buildsystem")
      {
      this->ProcessBuildsystem();
      }
    if (value["type"] == "target_info")
      {
      const char* language = 0;
      if (value.isMember("language"))
        {
        language = value["language"].asCString();
        }
      this->ProcessTargetInfo(
            value["target_name"].asString(),
            value["config"].asString(),
            language);
      }
    if (value["type"] == "file_info")
      {
      this->ProcessFileInfo(
            value["target_name"].asString(),
            value["config"].asString(),
            value["file_path"].asString());
      }
    }
}

void cmMetadataServer::WriteResponse(const Json::Value& jsonValue)
{
  Json::FastWriter writer;

  std::string result = "\n[== CMake MetaMagic ==[\n";
  result += writer.write(jsonValue);
  result += "]== CMake MetaMagic ==]\n";

  this->Writing = true;
  write_data((uv_stream_t *)&mStdout_pipe, result, on_stdout_write);
}

void cmMetadataServer::ProcessHandshake(std::string const& protocolVersion)
{
  // TODO: Handle version.
  (void)protocolVersion;

  this->State = Initializing;
  this->CMakeInstance = new cmake;
  std::set<std::string> emptySet;
  if(!this->CMakeInstance->GetState()->LoadCache(m_buildDir.c_str(),
                                                 true, emptySet, emptySet))
    {
    // Error;
    return;
    }

  const char* genName =
      this->CMakeInstance->GetState()
          ->GetInitializedCacheValue("CMAKE_GENERATOR");
  if (!genName)
    {
    // Error
    return;
    }

  const char* sourceDir =
      this->CMakeInstance->GetState()
          ->GetInitializedCacheValue("CMAKE_HOME_DIRECTORY");
  if (!sourceDir)
    {
    // Error
    return;
    }

  this->CMakeInstance->SetHomeDirectory(sourceDir);
  this->CMakeInstance->SetHomeOutputDirectory(m_buildDir);
  this->CMakeInstance->SetGlobalGenerator(
    this->CMakeInstance->CreateGlobalGenerator(genName));

  this->CMakeInstance->LoadCache();
  this->CMakeInstance->SetSuppressDevWarnings(true);
  this->CMakeInstance->SetWarnUninitialized(false);
  this->CMakeInstance->SetWarnUnused(false);
  this->CMakeInstance->PreLoadCMakeFiles();

  Json::Value obj = Json::objectValue;
  obj["progress"] = "initialized";

  this->WriteResponse(obj);

  // First not? But some other mode that aborts after ActualConfigure
  // and creates snapshots?
  this->CMakeInstance->Configure();

  obj["progress"] = "configured";

  this->WriteResponse(obj);

  if (!this->CMakeInstance->GetGlobalGenerator()->Compute())
    {
    // Error
    return;
    }

  obj["progress"] = "computed";

  this->WriteResponse(obj);

  auto srcDir = this->CMakeInstance->GetState()->GetSourceDirectory();

  Json::Value idleObj = Json::objectValue;
  idleObj["progress"] = "idle";
  idleObj["source_dir"] = srcDir;
  idleObj["binary_dir"] = this->CMakeInstance->GetState()->GetBinaryDirectory();
  idleObj["project_name"] = this->CMakeInstance->GetGlobalGenerator()
      ->GetLocalGenerators()[0]->GetProjectName();

  this->State = ProcessingRequests;

  this->WriteResponse(idleObj);
}

void cmMetadataServer::ProcessVersion()
{
  Json::Value obj = Json::objectValue;
  obj["version"] = CMake_VERSION;

  this->WriteResponse(obj);
}

void cmMetadataServer::ProcessBuildsystem()
{
  Json::Value root = Json::objectValue;
  Json::Value& obj = root["buildsystem"] = Json::objectValue;

  auto mf = this->CMakeInstance->GetGlobalGenerator()->GetMakefiles()[0];
  auto lg = this->CMakeInstance->GetGlobalGenerator()->GetLocalGenerators()[0];

  Json::Value& configs = obj["configs"] = Json::arrayValue;

  std::vector<std::string> configsVec;
  mf->GetConfigurations(configsVec);
  for (auto const& config : configsVec)
    {
    configs.append(config);
    }

  Json::Value& globalTargets = obj["globalTargets"] = Json::arrayValue;
  Json::Value& targets = obj["targets"] = Json::arrayValue;
  auto gens = this->CMakeInstance->GetGlobalGenerator()->GetLocalGenerators();

  auto firstMf =
      this->CMakeInstance->GetGlobalGenerator()->GetMakefiles()[0];
  auto firstTgts = firstMf->GetTargets();
  for (auto const& tgt : firstTgts)
    {
    if (tgt.second.GetType() == cmState::GLOBAL_TARGET)
      {
      globalTargets.append(tgt.second.GetName());
      }
    }

  for (auto const& gen : gens)
    {
    for (auto const& tgt : gen->GetGeneratorTargets())
      {
      if (tgt->IsImported())
        {
        continue;
        }
      if (tgt->GetType() == cmState::GLOBAL_TARGET)
        {
        continue;
        }
      Json::Value target = Json::objectValue;
      target["name"] = tgt->GetName();
      target["type"] = cmState::GetTargetTypeName(tgt->GetType());

      if (tgt->GetType() <= cmState::UTILITY)
        {
        auto lfbt = tgt->GetBacktrace();
        Json::Value bt = Json::arrayValue;
        for (auto const& lbtF : lfbt.FrameContexts())
          {
          Json::Value fff = Json::objectValue;
          fff["path"] = lbtF.FilePath;
          fff["line"] = (int)lbtF.Line;
          bt.append(fff);
          }
        target["backtrace"] = bt;
        if (tgt->GetType() < cmState::OBJECT_LIBRARY)
          {
//          std::string fp = (*ittgt)->GetFullPath(config, false, true);
//          targetValue["target_file"] = fp;
          }
        }
      // Should be list?
      target["projectName"] = lg->GetProjectName();
      targets.append(target);
      }
    }
  this->WriteResponse(root);
}

void cmMetadataServer::ProcessTargetInfo(std::string tgtName,
                                         std::string config,
                                         const char* language)
{
  Json::Value obj = Json::objectValue;
  Json::Value& root = obj["target_info"] = Json::objectValue;

  auto tgt =
      this->CMakeInstance->GetGlobalGenerator()->FindGeneratorTarget(tgtName);

  if (!tgt)
    {
    // Error
    return;
    }

  if (tgt->GetType() != cmState::GLOBAL_TARGET
      && tgt->GetType() != cmState::UTILITY
      && tgt->GetType() != cmState::OBJECT_LIBRARY)
    {
    root["build_location"] = tgt->GetLocation(config);
    if (tgt->HasImportLibrary())
      {
      root["build_implib"] = tgt->GetFullPath(config, true);
      }
    }

  std::vector<const cmSourceFile*> files;

  tgt->GetObjectSources(files, config);

  Json::Value& object_sources = root["object_sources"] = Json::arrayValue;
  Json::Value& generated_object_sources = root["generated_object_sources"] = Json::arrayValue;
  for (auto const& sf : files)
    {
    std::string filePath = sf->GetFullPath();
    if (sf->GetProperty("GENERATED"))
      {
      generated_object_sources.append(filePath);
      }
    else
      {
      object_sources.append(filePath);
      }
    }

  files.clear();

  tgt->GetHeaderSources(files, config);

  Json::Value& header_sources = root["header_sources"] = Json::arrayValue;
  Json::Value& generated_header_sources = root["generated_header_sources"] = Json::arrayValue;
  for (auto const& sf : files)
    {
    std::string filePath = sf->GetFullPath();
    if (sf->GetProperty("GENERATED"))
      {
      generated_header_sources.append(filePath);
      }
    else
      {
      header_sources.append(filePath);
      }
    }

  Json::Value& target_defines = root["compile_definitions"] = Json::arrayValue;

  std::string lang = language ? language : "C";

  std::vector<std::string> cdefs;
  tgt->GetCompileDefinitions(cdefs, config, lang);
  for (auto const& cdef : cdefs)
    {
    target_defines.append(cdef);
    }

  Json::Value& target_features = root["compile_features"] = Json::arrayValue;

  std::vector<std::string> features;
  tgt->GetCompileFeatures(cdefs, config);
  for (auto const& feature : features)
    {
    target_features.append(feature);
    }

  Json::Value& target_options = root["compile_options"] = Json::arrayValue;

  std::vector<std::string> options;
  tgt->GetCompileOptions(cdefs, config, lang);
  for (auto const& option : options)
    {
    target_options.append(option);
    }

  Json::Value& target_includes = root["include_directories"] = Json::arrayValue;

  std::vector<std::string> dirs;
  tgt->GetLocalGenerator()->GetIncludeDirectories(dirs, tgt, lang, config);
  for (auto const& dir : dirs)
    {
    target_includes.append(dir);
    }
  this->WriteResponse(obj);
}

void cmMetadataServer::ProcessFileInfo(std::string tgtName,
                                       std::string config,
                                       std::string file_path)
{
  auto tgt =
      this->CMakeInstance->GetGlobalGenerator()->FindGeneratorTarget(tgtName);

  if (!tgt)
    {
    // Error
    return;
    }

  Json::Value obj = Json::objectValue;

  std::vector<const cmSourceFile*> files;
  tgt->GetObjectSources(files, config);

  const cmSourceFile* file = 0;
  for (auto const& sf : files)
    {
    if (sf->GetFullPath() == file_path)
      {
      file = sf;
      break;
      }
    }

  if (!file)
    {
    // Error
    return;
    }

  // TODO: Get the includes/defines/flags for the file for this target.
  // There does not seem to be suitable API for that yet.

  this->WriteResponse(obj);
}
