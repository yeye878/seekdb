
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_UNLIKELY(x) (x)
#define OB_INLINE inline
#define UNUSED(x) (void)(x)
#define MIN(a,b) std::min(a,b)
#define OB_MIN(a,b) std::min(a,b)
#define MEMCMP std::memcmp
using ObCollationType = int;
struct ObCharsetInfo {};
namespace common { struct ObDatumAccessContext {}; }
using common::ObDatumAccessContext;
struct ObDatum {
  const char *ptr_;
  uint32_t len_;
  bool null_;
  bool is_null() const { return null_; }
};
static int ob_strnncoll_binary(const ObCharsetInfo *cs __attribute__((unused)),
                               const unsigned char *str, size_t s_len,
                               const unsigned char *t, size_t t_len,
                               bool t_is_prefix)
{
  size_t len = OB_MIN(s_len,t_len);
  int cmp = memcmp(str,t,len);
  return cmp ? cmp : (int)((t_is_prefix ? len : s_len) - t_len);
}
static int ob_strnncollsp_binary(const ObCharsetInfo *cs
                                 __attribute__((unused)),
                                 const unsigned char *str, size_t s_len,
                                 const unsigned char *t, size_t t_len,
                                 bool diff_if_only_endspace_difference
                                 __attribute__((unused)))
{
  return ob_strnncoll_binary(cs,str,s_len,t,t_len,0);
}
struct ObCharset {
  static int strcmpsp(int, const char *l, int64_t nl, const char *r, int64_t nr, bool spaces) {
    return ob_strnncollsp_binary(nullptr, reinterpret_cast<const unsigned char*>(l), nl,
        reinterpret_cast<const unsigned char*>(r), nr, spaces);
  }
};
namespace datum_cmp {
struct ObDatumStrCmpImpl
{
  static int OB_INLINE cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                           const ObCollationType cs, const bool with_end_space)
  {
    cmp_ret = ObCharset::strcmpsp(cs, l.ptr_, l.len_, r.ptr_, r.len_, with_end_space);
    cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    return OB_SUCCESS;
  }
};
template <int CS, bool Spaces> struct ObDatumStrCmp {
  static int cmp(const ObDatum &l, const ObDatum &r, int &res) {
    return ObDatumStrCmpImpl::cmp(l,r,res,CS,Spaces);
  }
};
}
template <ObCollationType CS_TYPE, bool WITH_END_SPACE, bool NULL_FIRST>
struct ObNullSafeDatumStrCmp
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx) {
    UNUSED(access_ctx);
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(l.is_null()) && OB_UNLIKELY(r.is_null())) {
      cmp_ret = 0;
    } else if (OB_UNLIKELY(l.is_null())) {
      cmp_ret = NULL_FIRST ? -1 : 1;
    } else if (OB_UNLIKELY(r.is_null())) {
      cmp_ret = NULL_FIRST ? 1 : -1;
    } else {
      ret = datum_cmp::ObDatumStrCmp<CS_TYPE, WITH_END_SPACE>::cmp(l, r, cmp_ret);
    }
    return ret;
  }
};static int compare_binary_retrieval_ids(
    const ObDatum &l, const ObDatum &r, int &cmp_ret,
    const common::ObDatumAccessContext *access_ctx)
{
  UNUSED(access_ctx);
  if (l.is_null() || r.is_null()) {
    cmp_ret = l.is_null() ? (r.is_null() ? 0 : -1) : 1;
  } else {
    // CS_TYPE_BINARY compares every byte, including trailing spaces and NULs.
    // The type/collation guard in init excludes text and other collations.
    const int64_t len = MIN(l.len_, r.len_);
    const int cmp = len > 0 ? MEMCMP(l.ptr_, r.ptr_, len) : 0;
    cmp_ret = cmp > 0 ? 1 : (cmp < 0 ? -1 : (l.len_ > r.len_) - (l.len_ < r.len_));
  }
  return OB_SUCCESS;
}
static uint64_t state = 0x12f75a031;
static uint64_t rng() { state ^= state<<13; state ^= state>>7; state ^= state<<17; return state; }
static int sign(int x) { return (x>0)-(x<0); }
static uint64_t cases = 0;
static void check(const ObDatum &l, const ObDatum &r) {
  int before=17, after=19;
  int a=ObNullSafeDatumStrCmp<63,false,true>::cmp(l,r,before,nullptr);
  int b=compare_binary_retrieval_ids(l,r,after,nullptr);
  if (a!=b || sign(before)!=sign(after)) { std::cerr << "mismatch " << cases << '\n'; std::abort(); }
  ++cases;
}
static ObDatum datum(const std::string &s) { return {s.data(), static_cast<uint32_t>(s.size()), false}; }
int main() {
  ObDatum null{nullptr,0,true};
  check(null,null);
  std::vector<std::string> edges={"", " ", "  ", "a", "a ", "a  ", "ab", std::string("a\0",2), std::string(1,'\0'), std::string(1,char(255))};
  for(const auto &l:edges) {
    check(null,datum(l)); check(datum(l),null);
    for(const auto &r:edges) check(datum(l),datum(r));
    for(uint32_t n=0;n<=l.size();++n) for(uint32_t m=0;m<=l.size();++m) check({l.data(),n,false},{l.data(),m,false});
  }
  for(int a=0;a<256;++a) for(int b=0;b<256;++b) {
    std::string l(1,char(a)),r(1,char(b)); check(datum(l),datum(r));
  }
  std::vector<std::string> pool;
  for(int i=0;i<4096;++i) {
    std::string s(rng()%81,'\0'); for(char &c:s)c=char(rng()); pool.push_back(s);
  }
  for(int i=0;i<600000;++i) {
    ObDatum l=datum(pool[rng()%pool.size()]),r=datum(pool[rng()%pool.size()]);
    if(rng()%97==0)l=null; if(rng()%97==0)r=null; check(l,r);
  }
  std::vector<int> old_order(pool.size()),new_order(pool.size());
  std::iota(old_order.begin(),old_order.end(),0); new_order=old_order;
  std::stable_sort(old_order.begin(),old_order.end(),[&](int l,int r){int res; ObNullSafeDatumStrCmp<63,false,true>::cmp(datum(pool[l]),datum(pool[r]),res,nullptr); return res<0;});
  std::stable_sort(new_order.begin(),new_order.end(),[&](int l,int r){int res; compare_binary_retrieval_ids(datum(pool[l]),datum(pool[r]),res,nullptr); return res<0;});
  assert(old_order==new_order);
  std::cout << "DIFFERENTIAL_CASES=" << cases << "\nSORTED_IDS=" << pool.size() << "\nMISMATCHES=0\n";
}
