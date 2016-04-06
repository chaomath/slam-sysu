#include "pointmatcher/PointMatcher.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <time.h>
#include <map>
#include <octomap/octomap.h>
#include <octomap/OcTree.h>
#include <octomap/ColorOcTree.h>
#include "icp.h"
#include "dbscan/dbscan.h"
#include "ransac/ransac.h"

using namespace std;
using namespace octomap;
using namespace Eigen;

#define MAX_RANGE -1

typedef PointMatcher<float>::TransformationParameters TransformMatrix;

// global
TransformMatrix TransAcc; // accumulated transform matrix
int total, progress; // for display progress

class CSVRow
{
    public:
        std::string const& operator[](std::size_t index) const
        {
            return m_data[index];
        }
        std::size_t size() const
        {
            return m_data.size();
        }
        void readNextRow(std::istream& str)
        {
            std::string         line;
            std::getline(str,line);

            std::stringstream   lineStream(line);
            std::string         cell;

            m_data.clear();
            while(std::getline(lineStream,cell,','))
            {
                m_data.push_back(cell);
            }
        }
    private:
        std::vector<std::string>    m_data;
};

std::istream& operator>>(std::istream& str,CSVRow& data)
{
    data.readNextRow(str);
    return str;
}

/**
 * read pointcloud from a csvfile
 * @param  filename [description]
 * @return          [description]
 */
Pointcloud readPointCloud(const char* filename) {
  Pointcloud Q;
  std::ifstream in(filename);
  CSVRow row;
  while(in >> row)
  {
    float x, y, z;
    x = atof(row[0].c_str());
    y = atof(row[1].c_str());
    z = atof(row[2].c_str());
    Q.push_back(x, y, z);
  }
  return Q;
}

// void extractGround(Pointcloud P, Pointcloud & ground, Pointcloud &nonGround) {
//   bool *isGround;
//   isGround = ransacFitPlane(P, 0.5, 6000, 500);
//   int size = P.size();
//   for (int i = 0; i < size; i++) {
//     if (isGround[i]) {
//       ground.push_back(P[i]);
//     } else {
//       nonGround.push_back(P[i]);
//     }
//   }
// }
void extractGround(Pointcloud P, Pointcloud & ground, Pointcloud &nonGround) {
  point3d upper, lower;
  P.calcBBX(lower, upper);
  point3d ground_upper(upper.x(), upper.x(), lower.z() + 1);
  point3d ground_lower(lower.x(), lower.x(), lower.z() + 7);
  ground.push_back(P);
  nonGround.push_back(P);
  ground.crop(lower, ground_upper);
  nonGround.crop(ground_lower, upper);
}

void getPointcloudFeatures(Pointcloud cluster, point3d &centroid) {
  int size = cluster.size();
  float x, y, z;
  for (int i = 0; i < size; i++) {
    x += cluster[i].x();
    y += cluster[i].y();
    z += cluster[i].z();
  }
  centroid = point3d(x/size, y/size, z/size);
}

void getInAndOutliners(Pointcloud P, bool * cs, Pointcloud &inliners, Pointcloud &outliners) {
  inliners.clear();
  outliners.clear();
  int size = P.size();
  int count = 0;
  for (int i = 0; i < size; i++) {
    if (cs[i]) {
      inliners.push_back(P[i]);
    } else {
      outliners.push_back(P[i]);
      count++;
    }
  }
}

/**
 * filter out dynamic points
 * @param  tree [description]
 * @param  P    [description]
 * @return      [pointcloud without dynamic points]
 */
Pointcloud dynamicFilter(ColorOcTree &tree, Pointcloud P, bool * cs) {
  long beginTime = clock();
  typedef std::map<int, Pointcloud> MAP;
  typedef std::pair<int, Pointcloud> PAIR;
  Pointcloud dcs; // dynamic candidates
  Pointcloud stationary;
  // for (Pointcloud::iterator it = P.begin(); it != P.end(); it++) {
  //   point3d endPoint((*it).x(), (*it).y(), (*it).z());
  //   OcTreeNode* node = tree.search (endPoint);
  //   if (node != NULL && node->getOccupancy() < 0.5) {
  //     dcs.push_back(endPoint);
  //   } else {
  //     stationary.push_back(endPoint);
  //   }
  // }
  getInAndOutliners(P, cs, stationary, dcs);

  int size = dcs.size();
  cout << "dynamic candidates size: " << size << "/" << P.size() << endl;
  int* clusters_idxs = new int[size];
  clusters_idxs = dbscan(dcs, 10, 1); // -min_points -epsilon
  MAP clusterMap;
  for (int i = 0; i < size; i++) {
    int cluster_idx = clusters_idxs[i];
    // if (cluster_idx == 0) continue; // noise
    MAP::iterator it = clusterMap.find(cluster_idx);
    float x = dcs[i].x();
    float y = dcs[i].y();
    float z = dcs[i].z();
    if (it != clusterMap.end()) {
      (it->second).push_back(point3d(x, y, z));
    } else {
      Pointcloud v;
      v.push_back(point3d(x, y, z));
      clusterMap.insert(PAIR(cluster_idx, v));
    }
  }

  for (MAP::iterator it = clusterMap.begin(); it != clusterMap.end(); it++) {
    Pointcloud cluster = it->second;
    int cluster_idx = it->first;
    point3d c; // centroid of the cluster
    getPointcloudFeatures(cluster, c);
    // if (cluster_idx == 0 || cluster.size() < 100 || cluster.size() > 1000 || c.z() > 1.5 || c.z() < -1) {
    if (cluster_idx == 0 || cluster.size() < 50) {
      stationary.push_back(cluster);
    } else {

      cout<< "cluster-" << it->first << " has " << cluster.size() << " points" << endl;
      // clear points
      Pointcloud tmp(P);
      point3d lowerBound, upperBound;
      cluster.calcBBX(lowerBound, upperBound);
      lowerBound -= point3d(0.5, 0.5, 0.5);
      upperBound += point3d(0.5, 0.5, 0.5);
      tmp.crop(lowerBound, upperBound);
      for (Pointcloud::iterator it = tmp.begin(); it != tmp.end(); it++) {
        // tree.updateNode((*it), false);
        ColorOcTreeNode* n = tree.updateNode((*it), true);
        n->setColor(255,0,0); // set color to red
      }
    }
  }

  long endTime = clock();
  char msg[100];
  sprintf(msg, "filter dynamic consume time: %.2f s.\n", (float)(endTime-beginTime)/1000000);
  cout << msg;

  cout << "dynamic points size: " << P.size() - stationary.size() << "/" << P.size() << endl;
  return stationary;
}

void initMap(ColorOcTree &tree, Pointcloud P) {
  // Pointcloud ground, PWithOutGround;
  // extractGround(P, ground, PWithOutGround);
  // P = PWithOutGround;

  for (Pointcloud::iterator it = P.begin(); it != P.end(); it++) {
    tree.updateNode((*it).x(), (*it).y(), (*it).z(), true);
    tree.setNodeColor((*it).x(), (*it).y(), (*it).z(), 0, 0, 255);
  }
}

Pointcloud updateMap(ColorOcTree &tree, Pointcloud P, Pointcloud lastP) {
  long beginTime = clock();
  Pointcloud ground, PWithOutGround, P_;
  // icp and dynamic dectction should be implemented without ground points
  bool * cs = new bool[P.size()];
  P = icp(lastP, P, TransAcc, cs);
  // dynamic detection
  // extractGround(P, ground, PWithOutGround);
  P_ = dynamicFilter(tree, P, cs);
  // P_.push_back(ground);
  P = P_;

  for (Pointcloud::iterator it = P.begin(); it != P.end(); it++) {
    tree.updateNode((*it), true);
    tree.setNodeColor((*it).x(), (*it).y(), (*it).z(), 0, 0, 255);
    // if (cs[i]) {
    //   tree.setNodeColor((*it).x(), (*it).y(), (*it).z(), 0, 0, 255);
    // } else {
    //   tree.setNodeColor((*it).x(), (*it).y(), (*it).z(), 255, 0, 0);
    // }
  } // color
  long endTime = clock();
  char msg[100];
  sprintf(msg, "frame %d/%d completed, consumed time: %.2f s.\n", progress, total, (float)(endTime-beginTime)/1000000);
  cout << msg;

  delete[] cs;
  return P;
}

int main(int argc, char** argv) {
  ColorOcTree tree (0.1);  // create empty tree with resolution 0.1
  int from  = atoi(argv[1]);
  int to = atoi(argv[2]);
  int step = atoi(argv[3]);
  string path = argv[4];

  // init
  char baseFile[50];
  sprintf(baseFile, "%s (Frame %04d).csv", path.c_str(), from);
  Pointcloud base = readPointCloud(baseFile);
  initMap(tree, base);
  TransAcc.resize(4, 4);
  TransAcc.setIdentity();
  total = (int) (to - from) / step;
  progress = 1;

	Pointcloud P, lastP;
  lastP = base;
  char file[50];
  for (int i = from + 1; i <= to; i += step) {
    sprintf(file, "%s (Frame %04d).csv", path.c_str(), i);
    P = readPointCloud(file);
    lastP = updateMap(tree, P, lastP);
    progress++;
  }

  // string result = "map.bt";
  // tree.writeBinary(result);

  string result = "map.ot";
  tree.write(result);

  cout << "wrote example file " << result << endl;

  return 0;
}
