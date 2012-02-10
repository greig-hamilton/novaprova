#include "walker.hxx"
#include "enumerations.hxx"
#include "state.hxx"

namespace spiegel { namespace dwarf {
using namespace std;

int
walker_t::read_entry()
{
    /* Refs are offsets relative to the start of the
     * compile unit header, but the reader starts
     * at the first byte after that header. */
    size_t offset = reader_.get_offset();
    uint32_t acode;

    if (!reader_.read_uleb128(acode))
    {
	return EOF;    // end of section
    }

    if (!acode)
    {
	if (!level_)
	    return EOF;   // end of subtree in scope
	level_--;
	return 0;
    }

    const abbrev_t *a = compile_unit_.get_abbrev(acode);
    if (!a)
    {
	// TODO: bad DWARF info - throw an exception
	fatal("XXX wtf - no abbrev for code 0x%x\n", acode);
    }

    entry_.setup(offset, level_, a);

    vector<abbrev_t::attr_spec_t>::const_iterator i;
    for (i = a->attr_specs.begin() ; i != a->attr_specs.end() ; ++i)
    {
	switch (i->form)
	{
	case DW_FORM_data1:
	    {
		uint8_t v;
		if (!reader_.read_u8(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_uint32(v));
		break;
	    }
	case DW_FORM_data2:
	    {
		uint16_t v;
		if (!reader_.read_u16(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_uint32(v));
		break;
	    }
	case DW_FORM_data4:
	    {
		uint32_t v;
		if (!reader_.read_u32(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_uint32(v));
		break;
	    }
	case DW_FORM_data8:
	    {
		uint64_t v;
		if (!reader_.read_u64(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_uint64(v));
		break;
	    }
	case DW_FORM_udata:
	    {
		uint32_t v;
		if (!reader_.read_uleb128(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_uint32(v));
		break;
	    }
	case DW_FORM_sdata:
	    {
		int32_t v;
		if (!reader_.read_sleb128(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_sint32(v));
		break;
	    }
	case DW_FORM_addr:
	    {
		if (sizeof(unsigned long) == 4)
		{
		    uint32_t v;
		    if (!reader_.read_u32(v))
			return EOF;
		    entry_.add_attribute(i->name, value_t::make_uint32(v));
		}
		else if (sizeof(unsigned long) == 8)
		{
		    uint64_t v;
		    if (!reader_.read_u64(v))
			return EOF;
		    entry_.add_attribute(i->name, value_t::make_uint64(v));
		}
		else
		{
		    fatal("Strange addrsize %u", sizeof(unsigned long));
		}
		break;
	    }
	case DW_FORM_flag:
	    {
		uint8_t v;
		if (!reader_.read_u8(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_uint32(v));
		break;
	    }
	case DW_FORM_ref1:
	    {
		uint8_t off;
		if (!reader_.read_u8(off))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_ref(off));
		break;
	    }
	case DW_FORM_ref2:
	    {
		uint16_t off;
		if (!reader_.read_u16(off))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_ref(off));
		break;
	    }
	case DW_FORM_ref4:
	    {
		uint32_t off;
		if (!reader_.read_u32(off))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_ref(off));
		break;
	    }
	case DW_FORM_ref8:
	    {
		uint64_t off;
		if (!reader_.read_u64(off))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_ref(off));
		break;
	    }
	case DW_FORM_string:
	    {
		const char *v;
		if (!reader_.read_string(v))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_string(v));
		break;
	    }
	case DW_FORM_strp:
	    {
		uint32_t off;
		if (!reader_.read_u32(off))
		    return EOF;
		const char *v = state_.sections_[DW_sec_str].offset_as_string(off);
		if (!v)
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_string(v));
		break;
	    }
	case DW_FORM_block1:
	    {
		uint8_t len;
		const unsigned char *v;
		if (!reader_.read_u8(len) ||
		    !reader_.read_bytes(v, len))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_bytes(v, len));
		break;
	    }
	case DW_FORM_block2:
	    {
		uint16_t len;
		const unsigned char *v;
		if (!reader_.read_u16(len) ||
		    !reader_.read_bytes(v, len))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_bytes(v, len));
		break;
	    }
	case DW_FORM_block4:
	    {
		uint32_t len;
		const unsigned char *v;
		if (!reader_.read_u32(len) ||
		    !reader_.read_bytes(v, len))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_bytes(v, len));
		break;
	    }
	case DW_FORM_block:
	    {
		uint32_t len;
		const unsigned char *v;
		if (!reader_.read_uleb128(len) ||
		    !reader_.read_bytes(v, len))
		    return EOF;
		entry_.add_attribute(i->name, value_t::make_bytes(v, len));
		break;
	    }
	default:
	    // TODO: bad DWARF info - throw an exception
	    fatal("XXX can't handle %s\n",
		  formvals.to_name(i->form));
	}
    }

    if (a->children)
	level_++;

    return 1;
}

// level_ is the level relative to the very first entry
// at which the walker was initialised or seeked to,
// of the next entry to be read from the file

bool
walker_t::move_preorder()
{
    int r;
    do
    {
	if ((r = read_entry()) == EOF)
	    return false;
    } while (!r);
    return true;
}

bool
walker_t::move_to_sibling()
{
    // TODO: use the DW_AT_sibling attribute if present
    unsigned target_level = entry_.get_level();
    int r;
    do
    {
	if ((r = read_entry()) == EOF)
	    return false;
    } while (r && entry_.get_level() > target_level);
    return !!r;
}

bool
walker_t::move_to_children()
{
    return (entry_.has_children() && read_entry() != EOF);
}

// close namespace
} }