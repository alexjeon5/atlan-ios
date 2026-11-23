// SPDX-License-Identifier: GPL-2.0-or-later
//
// The library's table of the decoder's forms (semantics_internal.hpp): the
// operation of each (form_operation.hpp) and what only the library needs to
// execute it.

#include "cpu/arm64/aarch32/semantics_internal.hpp"

#include <array>
#include <string_view>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

[[nodiscard]] const std::array<OpInfo, kFormCount> &table()
{
	static const auto built = [] {
		std::array<OpInfo, kFormCount> result{};
		for (std::size_t index = 0; index < kFormCount; ++index) {
			const std::string_view name = kForms[index].mnemonic;
			const std::string_view form = kForms[index].name;
			OpInfo info{form_operation(static_cast<Form>(index))};
			// Only integer, memory and control handlers are imported.
			if (info.op == Op::vldr || info.op == Op::vstr || info.op == Op::vldm
				|| info.op == Op::vstm || info.op == Op::vld || info.op == Op::vst)
				info.op = Op::none;
			/* BLX (immediate) shares BL's encoding file and links the same way. */
			if (info.op == Op::blx && form.starts_with("BL_i"))
				info.op = Op::bl;
			if (info.op == Op::vld || info.op == Op::vst) {
				/* VLDn_m_ (multiple structures), VLDn_1_ (one lane) and
				 * VLDn_a_ (all lanes), after the mnemonic's digit. */
				info.structs = static_cast<std::uint8_t>(name.back() - '0');
				info.structure = form.find("_1_") != std::string_view::npos
					? StructureKind::single_lane
					: form.find("_a_") != std::string_view::npos
						? StructureKind::all_lanes
						: StructureKind::multiple;
			}
			if (info.op == Op::ldm || info.op == Op::stm) {
				info.stack = form.starts_with("PUSH") || form.starts_with("POP");
				if (form.starts_with("LDMDA") || form.starts_with("STMDA"))
					info.transfer = Mode::da;
				else if (form.starts_with("LDMDB") || form.starts_with("STMDB")
					|| form.starts_with("PUSH"))
					info.transfer = Mode::db;
				else if (form.starts_with("LDMIB") || form.starts_with("STMIB"))
					info.transfer = Mode::ib;
			}
			result[index] = info;
		}
		return result;
	}();
	return built;
}

} // namespace

const OpInfo &op_info(Form form) noexcept
{
	return table()[static_cast<std::size_t>(form)];
}

} // namespace uml::cpu::arm64::aarch32::detail
