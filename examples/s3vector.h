#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <optional>
#include <cstdint>

// Vector
struct Vector {
  std::string key;
  std::vector<float> data;
  std::string metadata;
};

// Vector Result (for query responses)
struct VectorResult {
  std::string key;
  std::optional<std::vector<float>> data;  // Optional, included if returnData is true
  std::optional<std::string> metadata;    // Optional, included if returnMetadata is true
  std::optional<float> distance;   // Optional, included if returnDistance is true

};

// ================================
// CREATE OPERATIONS
// ================================

int CreateVectorBucket(const std::string& vectorBucketName, std::string& vectorBucketArn);

// CreateIndex
struct CreateIndexInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  const std::string dataType = "float32";
  uint16_t dimension = 1;               // 1-4096
  std::string distanceMetric;  // "cosine" or "euclidean"
  std::vector<std::string> metadataConfiguration;
};

int CreateIndex(const CreateIndexInput& input, std::string& indexArn);

// ================================
// DELETE OPERATIONS
// ================================

// DeleteVectorBucket
struct DeleteVectorBucketInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
};

int DeleteVectorBucket(const DeleteVectorBucketInput& input);

// DeleteIndex
struct DeleteIndexInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
};

int DeleteIndex(const DeleteIndexInput& input);

// DeleteVectorBucketPolicy
struct DeleteVectorBucketPolicyInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
};

int DeleteVectorBucketPolicy(const DeleteVectorBucketPolicyInput& input);

// DeleteVectors
struct DeleteVectorsInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
  std::vector<std::string> keys;
};

int DeleteVectors(const DeleteVectorsInput& input);

// ================================
// GET OPERATIONS
// ================================

// GetVectorBucket
struct GetVectorBucketInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
};

struct GetVectorBucketOutput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string creationDate;
};

int GetVectorBucket(const GetVectorBucketInput& input, GetVectorBucketOutput& output);

// GetIndex
struct GetIndexInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
};

struct GetIndexOutput {
  std::string indexName;
  std::string indexArn;
  std::string dataType;
  uint16_t dimension;
  std::string distanceMetric;
  std::string creationDate;
  std::vector<std::string> metadataConfiguration;
};

int GetIndex(const GetIndexInput& input, GetIndexOutput& output);

// GetVectorBucketPolicy
struct GetVectorBucketPolicyInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
};

int GetVectorBucketPolicy(const GetVectorBucketPolicyInput& input, std::string& policy);

// GetVectors
struct GetVectorsInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
  std::vector<std::string> keys;
  bool returnData;
  bool returnMetadata;
};

int GetVectors(const GetVectorsInput& input, std::vector<VectorResult>& vectors);

// ================================
// LIST OPERATIONS
// ================================

// ListVectorBuckets
struct ListVectorBucketsInput {
  std::string nextToken;
  size_t maxResults;
};

struct VectorBucketInfo {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string creationDate;
};

struct ListVectorBucketsOutput {
  std::vector<VectorBucketInfo> vectorBuckets;
  std::string nextToken;
};

int ListVectorBuckets(const ListVectorBucketsInput& input, ListVectorBucketsOutput& output);

// ListIndexes
struct ListIndexesInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string nextToken;
  size_t maxResults;
};

struct IndexInfo {
  std::string indexName;
  std::string indexArn;
  std::string dataType;
  uint16_t dimension;
  std::string distanceMetric;
  std::string creationDate;
};

struct ListIndexesOutput {
  std::vector<IndexInfo> indexes;
  std::string nextToken;
};

int ListIndexes(const ListIndexesInput& input, ListIndexesOutput& output);

// ListVectors
struct ListVectorsInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
  std::string nextToken;
  size_t maxResults;
  bool returnData;
  bool returnMetadata;
};

struct ListVectorsOutput {
  std::vector<VectorResult> vectors;
  std::string nextToken;
};

int ListVectors(const ListVectorsInput& input, ListVectorsOutput& output);

// ================================
// PUT OPERATIONS
// ================================

// PutVectorBucketPolicy
struct PutVectorBucketPolicyInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string policy;  // JSON policy document as string
};

int PutVectorBucketPolicy(const PutVectorBucketPolicyInput& input);

// PutVectors
struct PutVectorsInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
  std::vector<Vector> vectors;

};

int PutVectors(const PutVectorsInput& input);

// ================================
// QUERY OPERATIONS
// ================================

// QueryVectors
struct QueryVectorsInput {
  std::string vectorBucketName;
  std::string vectorBucketArn;
  std::string indexName;
  std::string indexArn;
  std::vector<float>  queryVector;
  size_t topK;                     // Up to 30
  std::string filter;           // Optional metadata filter
  bool returnDistance;
  bool returnMetadata;
  bool returnData;              // Not mentioned in docs but logical
};

int QueryVectors(const QueryVectorsInput& input, std::vector<VectorResult>& vectors);

