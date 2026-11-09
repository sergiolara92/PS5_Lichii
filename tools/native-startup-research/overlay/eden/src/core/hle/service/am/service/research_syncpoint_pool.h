// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <utility>
namespace Service::AM::ResearchSyncpoint {
// Real owned resources; atomic rollback if the full HLE budget is unavailable.
template <typename T, std::size_t Capacity> class OwnedPool {
public:
 explicit OwnedPool(std::function<void(T*)> release):release{std::move(release)}{}
 ~OwnedPool(){Clear();}
 OwnedPool(const OwnedPool&)=delete;
 OwnedPool& operator=(const OwnedPool&)=delete;
 template<typename Make> bool Initialize(Make make){
  if(count!=0)return false;
  while(count<Capacity){T* item=make();if(!item){Clear();return false;}slots[count++]=item;}
  return true;
 }
 std::size_t Size()const{return count;}
 std::size_t ActiveCount()const{return active;}
 bool SetActiveCount(std::size_t value){if(value>count)return false;active=value;return true;}
 T* Get(std::size_t index)const{return index<active?slots[index]:nullptr;}
private:
 void Clear(){active=0;while(count){T* item=slots[--count];slots[count]=nullptr;release(item);}}
 std::array<T*,Capacity> slots{};
 std::size_t count{},active{};
 std::function<void(T*)> release;
};
}
