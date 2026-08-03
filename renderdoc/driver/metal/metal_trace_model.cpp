/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "metal_trace_model.h"
#include "common/formatting.h"
#include "serialise/rdcfile.h"

namespace MetalTrace
{
namespace
{
static bool WriteHeader(StreamWriter &writer, const ContainerHeader &header)
{
  return writer.Write(header.magic) && writer.Write(header.containerVersion) &&
         writer.Write((uint32_t)header.sourceKind) && writer.Write(header.manifestVersion) &&
         writer.Write(header.indexVersion);
}

static bool ReadHeader(StreamReader &reader, ContainerHeader &header)
{
  uint32_t sourceKind = 0;
  bool success = reader.Read(header.magic) && reader.Read(header.containerVersion) &&
                 reader.Read(sourceKind) && reader.Read(header.manifestVersion) &&
                 reader.Read(header.indexVersion);
  header.sourceKind = (SourceKind)sourceKind;
  return success;
}

static bool WriteBytes(StreamWriter &writer, const byte *data, size_t size)
{
  uint64_t length = size;
  return writer.Write(length) && writer.Write(data, length);
}

static bool ReadBytes(StreamReader &reader, bytebuf &data)
{
  uint64_t length = 0;
  if(!reader.Read(length) || length > 1024ULL * 1024ULL * 1024ULL ||
     reader.GetOffset() > reader.GetSize() || length > reader.GetSize() - reader.GetOffset())
    return false;
  data.resize((size_t)length);
  return reader.Read(data.data(), length);
}

static bool WriteString(StreamWriter &writer, const rdcstr &str)
{
  return WriteBytes(writer, (const byte *)str.data(), str.size());
}

static bool ReadString(StreamReader &reader, rdcstr &str)
{
  bytebuf data;
  if(!ReadBytes(reader, data))
    return false;
  str.assign((const char *)data.data(), data.size());
  return true;
}

static bool WriteStringArray(StreamWriter &writer, const rdcarray<rdcstr> &strings)
{
  uint64_t count = strings.size();
  if(!writer.Write(count))
    return false;
  for(const rdcstr &str : strings)
    if(!WriteString(writer, str))
      return false;
  return true;
}

static bool ReadStringArray(StreamReader &reader, rdcarray<rdcstr> &strings)
{
  uint64_t count = 0;
  if(!reader.Read(count) || count > 1024ULL * 1024ULL)
    return false;
  strings.resize((size_t)count);
  for(rdcstr &str : strings)
    if(!ReadString(reader, str))
      return false;
  return true;
}

static bool WriteNode(StreamWriter &writer, const Node &node)
{
  return writer.Write(node.stableId) && writer.Write((uint32_t)node.kind) &&
         WriteString(writer, node.path) && WriteString(writer, node.name) &&
         WriteString(writer, node.label) && WriteString(writer, node.objectName) &&
         WriteStringArray(writer, node.values) && writer.Write(node.canGo) &&
         writer.Write(node.canInfo) && writer.Write(node.canFetch) && writer.Write(node.byteSize);
}

static bool ReadNode(StreamReader &reader, Node &node)
{
  uint32_t kind = 0;
  bool success = reader.Read(node.stableId) && reader.Read(kind) && ReadString(reader, node.path) &&
                 ReadString(reader, node.name) && ReadString(reader, node.label) &&
                 ReadString(reader, node.objectName) && ReadStringArray(reader, node.values) &&
                 reader.Read(node.canGo) && reader.Read(node.canInfo) &&
                 reader.Read(node.canFetch) && reader.Read(node.byteSize);
  node.kind = (NodeKind)kind;
  return success && kind <= (uint32_t)NodeKind::Binding && node.stableId != 0;
}

static bool WriteIndexV2(StreamWriter &writer, const Index &index)
{
  if(!WriteString(writer, index.toolVersion))
    return false;

  uint64_t nodeCount = index.nodes.size();
  if(!writer.Write(nodeCount))
    return false;
  for(const Node &node : index.nodes)
    if(!WriteNode(writer, node))
      return false;

  uint64_t listingCount = index.rawListings.size();
  if(!writer.Write(listingCount))
    return false;
  for(const RawListing &listing : index.rawListings)
    if(!WriteString(writer, listing.path) || !WriteString(writer, listing.json))
      return false;

  return true;
}

static bool ReadIndexV2(StreamReader &reader, Index &index)
{
  if(!ReadString(reader, index.toolVersion))
    return false;

  uint64_t nodeCount = 0;
  if(!reader.Read(nodeCount) || nodeCount > 1024ULL * 1024ULL)
    return false;
  index.nodes.resize((size_t)nodeCount);
  for(Node &node : index.nodes)
    if(!ReadNode(reader, node))
      return false;

  uint64_t listingCount = 0;
  if(!reader.Read(listingCount) || listingCount > 1024ULL * 1024ULL)
    return false;
  index.rawListings.resize((size_t)listingCount);
  for(RawListing &listing : index.rawListings)
    if(!ReadString(reader, listing.path) || !ReadString(reader, listing.json))
      return false;

  return true;
}

static bool WriteIndexV3(StreamWriter &writer, const Index &index)
{
  return WriteIndexV2(writer, index) && WriteString(writer, index.bufferFetchUnavailableReason);
}

static bool ReadIndexV3(StreamReader &reader, Index &index)
{
  return ReadIndexV2(reader, index) && ReadString(reader, index.bufferFetchUnavailableReason);
}

static bool WriteIndexV4(StreamWriter &writer, const Index &index)
{
  return WriteIndexV3(writer, index) && WriteString(writer, index.argumentBufferResolution) &&
         WriteString(writer, index.argumentBufferUnavailableReason);
}

static bool ReadIndexV4(StreamReader &reader, Index &index)
{
  return ReadIndexV3(reader, index) && ReadString(reader, index.argumentBufferResolution) &&
         ReadString(reader, index.argumentBufferUnavailableReason);
}

static bool WriteIndexV5(StreamWriter &writer, const Index &index)
{
  if(!WriteIndexV4(writer, index))
    return false;

  uint64_t infoCount = index.nodeInfos.size();
  if(!writer.Write(infoCount))
    return false;
  for(const NodeInfo &info : index.nodeInfos)
    if(!WriteString(writer, info.path) || !WriteStringArray(writer, info.keys) ||
       !WriteStringArray(writer, info.values))
      return false;
  return true;
}

static bool ReadIndexV5(StreamReader &reader, Index &index)
{
  if(!ReadIndexV4(reader, index))
    return false;

  uint64_t infoCount = 0;
  if(!reader.Read(infoCount) || infoCount > 1024ULL * 1024ULL)
    return false;
  index.nodeInfos.resize((size_t)infoCount);
  for(NodeInfo &info : index.nodeInfos)
    if(!ReadString(reader, info.path) || !ReadStringArray(reader, info.keys) ||
       !ReadStringArray(reader, info.values) || info.keys.size() != info.values.size())
      return false;
  return true;
}

static RDResult FinishSection(StreamWriter *writer)
{
  writer->Finish();
  RDResult result = writer->GetError();
  delete writer;
  return result;
}

static RDResult ValidateHeader(const ContainerHeader &header)
{
  if(header.magic != ContainerMagic)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace container magic is invalid");
  if(header.containerVersion != ContainerVersion || header.manifestVersion != ManifestVersion ||
     header.indexVersion < MinimumIndexVersion || header.indexVersion > IndexVersion)
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "Unsupported Metal trace container versions %u/%u/%u",
                        header.containerVersion, header.manifestVersion, header.indexVersion);
  return ResultCode::Succeeded;
}
};    // namespace

RDResult WriteThinRDC(RDCFile *rdc, const Manifest &manifest, const Index &index)
{
  if(rdc == NULL)
    return ResultCode::InvalidParameter;

  SectionProperties props;
  props.type = SectionType::FrameCapture;
  props.version = ContainerVersion;
  StreamWriter *writer = rdc->WriteSection(props);
  WriteHeader(*writer, manifest.header);
  RDResult result = FinishSection(writer);
  if(result != ResultCode::Succeeded)
    return result;

  props = {};
  props.type = SectionType::Unknown;
  props.name = ManifestSectionName;
  props.version = ManifestVersion;
  writer = rdc->WriteSection(props);
  WriteHeader(*writer, manifest.header);
  writer->Write((uint64_t)manifest.capabilities);
  WriteString(*writer, manifest.sourcePath);
  result = FinishSection(writer);
  if(result != ResultCode::Succeeded)
    return result;

  props.name = IndexSectionName;
  props.version = manifest.header.indexVersion;
  writer = rdc->WriteSection(props);
  writer->Write(IndexMagic);
  writer->Write(manifest.header.indexVersion);
  WriteString(*writer, index.actionName);
  WriteString(*writer, index.resourceName);
  WriteBytes(*writer, index.resourceData.data(), index.resourceData.size());
  if(manifest.header.indexVersion >= 5)
    WriteIndexV5(*writer, index);
  else if(manifest.header.indexVersion >= 4)
    WriteIndexV4(*writer, index);
  else if(manifest.header.indexVersion >= 3)
    WriteIndexV3(*writer, index);
  else if(manifest.header.indexVersion >= 2)
    WriteIndexV2(*writer, index);
  return FinishSection(writer);
}

RDResult ReadContainerHeader(RDCFile *rdc, ContainerHeader &header)
{
  if(rdc == NULL)
    return ResultCode::InvalidParameter;
  int section = rdc->SectionIndex(SectionType::FrameCapture);
  if(section < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace container has no frame section");

  StreamReader *reader = rdc->ReadSection(section);
  bool success = ReadHeader(*reader, header);
  RDResult result = reader->GetError();
  delete reader;
  if(!success || result != ResultCode::Succeeded)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace container header is truncated");
  return ValidateHeader(header);
}

RDResult ReadManifest(RDCFile *rdc, Manifest &manifest)
{
  int section = rdc ? rdc->SectionIndex(ManifestSectionName) : -1;
  if(section < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace container has no manifest");

  manifest = {};
  if(rdc->GetSectionProperties(section).version != ManifestVersion)
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "Unsupported Metal trace manifest section version %u",
                        (uint32_t)rdc->GetSectionProperties(section).version);

  StreamReader *reader = rdc->ReadSection(section);
  uint64_t capabilities = 0;
  bool success = ReadHeader(*reader, manifest.header) && reader->Read(capabilities) &&
                 ReadString(*reader, manifest.sourcePath);
  RDResult streamResult = reader->GetError();
  delete reader;
  if(!success || streamResult != ResultCode::Succeeded)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace manifest is malformed");
  manifest.capabilities = (Capability)capabilities;
  return ValidateHeader(manifest.header);
}

RDResult ReadIndex(RDCFile *rdc, Index &index)
{
  int section = rdc ? rdc->SectionIndex(IndexSectionName) : -1;
  if(section < 0)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace container has no index");

  index = {};
  uint64_t sectionVersion = rdc->GetSectionProperties(section).version;
  if(sectionVersion < MinimumIndexVersion || sectionVersion > IndexVersion)
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "Unsupported Metal trace index section version %llu",
                        (unsigned long long)sectionVersion);

  StreamReader *reader = rdc->ReadSection(section);
  uint32_t magic = 0, version = 0;
  bool success = reader->Read(magic) && reader->Read(version) &&
                 ReadString(*reader, index.actionName) && ReadString(*reader, index.resourceName) &&
                 ReadBytes(*reader, index.resourceData);
  if(success && version >= 5)
    success = ReadIndexV5(*reader, index);
  else if(success && version >= 4)
    success = ReadIndexV4(*reader, index);
  else if(success && version >= 3)
    success = ReadIndexV3(*reader, index);
  else if(success && version >= 2)
    success = ReadIndexV2(*reader, index);
  RDResult streamResult = reader->GetError();
  delete reader;
  if(!success || streamResult != ResultCode::Succeeded || magic != IndexMagic)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted, "Metal trace index is malformed");
  if(version < MinimumIndexVersion || version > IndexVersion)
    RETURN_ERROR_RESULT(ResultCode::APIIncompatibleVersion,
                        "Unsupported Metal trace index version %u", version);
  if(version != sectionVersion)
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                        "Metal trace index section and payload versions disagree");
  return ResultCode::Succeeded;
}
};    // namespace MetalTrace
