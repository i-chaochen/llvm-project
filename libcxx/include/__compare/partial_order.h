//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___COMPARE_PARTIAL_ORDER
#define _LIBCPP___COMPARE_PARTIAL_ORDER

#include <__compare/compare_three_way.h>
#include <__compare/ordering.h>
#include <__compare/weak_order.h>
#include <__config>
#include <__type_traits/decay.h>
#include <__type_traits/is_same.h>
#include <__utility/forward.h>
#include <__utility/priority_tag.h>

#ifndef _LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER > 17

// [cmp.alg]
namespace __partial_order {
    struct __fn {
        template<class _Tp, class _Up>
            requires is_same_v<decay_t<_Tp>, decay_t<_Up>>
        _LIBCPP_HIDE_FROM_ABI static constexpr auto
        __go(_Tp&& __t, _Up&& __u, __priority_tag<2>)
            noexcept(noexcept(partial_ordering(partial_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u)))))
            -> decltype(      partial_ordering(partial_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))))
            { return          partial_ordering(partial_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))); }

        template<class _Tp, class _Up>
            requires is_same_v<decay_t<_Tp>, decay_t<_Up>>
        _LIBCPP_HIDE_FROM_ABI static constexpr auto
        __go(_Tp&& __t, _Up&& __u, __priority_tag<1>)
            noexcept(noexcept(partial_ordering(compare_three_way()(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u)))))
            -> decltype(      partial_ordering(compare_three_way()(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))))
            { return          partial_ordering(compare_three_way()(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))); }

        template<class _Tp, class _Up>
            requires is_same_v<decay_t<_Tp>, decay_t<_Up>>
        _LIBCPP_HIDE_FROM_ABI static constexpr auto
        __go(_Tp&& __t, _Up&& __u, __priority_tag<0>)
            noexcept(noexcept(partial_ordering(_VSTD::weak_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u)))))
            -> decltype(      partial_ordering(_VSTD::weak_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))))
            { return          partial_ordering(_VSTD::weak_order(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u))); }

        template<class _Tp, class _Up>
        _LIBCPP_HIDE_FROM_ABI constexpr auto operator()(_Tp&& __t, _Up&& __u) const
            noexcept(noexcept(__go(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u), __priority_tag<2>())))
            -> decltype(      __go(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u), __priority_tag<2>()))
            { return          __go(_VSTD::forward<_Tp>(__t), _VSTD::forward<_Up>(__u), __priority_tag<2>()); }
    };
} // namespace __partial_order

inline namespace __cpo {
    inline constexpr auto partial_order = __partial_order::__fn{};
} // namespace __cpo

#endif // _LIBCPP_STD_VER > 17

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___COMPARE_PARTIAL_ORDER
