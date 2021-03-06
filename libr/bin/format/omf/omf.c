
#include "omf.h"

static int is_valid_omf_type(ut8 type) {
	int ct = 0;
	ut8 types[] = {
		OMF_THEADR, OMF_LHEADR, OMF_COMENT, OMF_MODEND, OMF_MODEND32,
		OMF_EXTDEF, OMF_PUBDEF, OMF_PUBDEF32, OMF_LINNUM,
		OMF_LINNUM32, OMF_LNAMES, OMF_LNAMES, OMF_SEGDEF,
		OMF_SEGDEF32, OMF_GRPDEF, OMF_FIXUPP, OMF_FIXUPP32,
		OMF_LEDATA, OMF_LEDATA32, OMF_LIDATA, OMF_LIDATA32,
		OMF_COMDEF, OMF_BAKPAT, OMF_BAKPAT32, OMF_LEXTDEF,
		OMF_LPUBDEF, OMF_LPUBDEF32, OMF_LCOMDEF, OMF_CEXTDEF,
		OMF_COMDAT, OMF_COMDAT32, OMF_LINSYM, OMF_LINSYM32,
		OMF_ALIAS, OMF_NBKPAT, OMF_NBKPAT32, OMF_LLNAMES, OMF_VERNUM,
		OMF_VENDEXT, 0};
	for (; types[ct]; ct++)
		if (type == types[ct])
			return R_TRUE;

	eprintf ("Invalid record type\n");
	
	return R_FALSE;
}

int r_bin_checksum_omf_ok(const char *buf, ut64 buf_size) {
	ut16 size;
	ut8 checksum = 0;

	if (buf_size < 3) {
		eprintf ("Invalid record (too short)\n");
		return R_FALSE;
	}
	size = *((ut16 *)(buf + 1));
	if (buf_size < size + 3) {
		eprintf ("Invalid record (too short)\n");
		return R_FALSE;
	}
	//Some compiler set checksum to 0
	if (!buf[2 + size])
		return R_TRUE;

	size += 3;
	for (; size; size--) {
		if (buf_size < size) {
			eprintf ("Invalid record (too short)\n");
			return R_FALSE;
		}
		checksum += buf[size - 1];
	}
	if (checksum)
		eprintf ("Invalid record checksum\n");

	return !checksum ? R_TRUE : R_FALSE;
}

static ut16 omf_get_idx(const char *buf) {
	ut16 idx;

	if (*buf & 0x80)
		idx = (*buf & 0x7f) * 0x100 + buf[1];
	else idx = *buf;
	return idx;
}

static void free_lname(OMF_multi_datas *lname) {
	ut32 ct = 0;

	while (ct < lname->nb_elem) {
		R_FREE (((char **)lname->elems)[ct]);
		ct++;
	}
	R_FREE (lname->elems);
	R_FREE (lname);
}

static int load_omf_lnames(OMF_record *record, const char *buf, ut64 buf_size) {
	ut32 tmp_size = 0;
	ut32 ct_name = 0;
	OMF_multi_datas *ret = NULL;
	char **names;

	if (!(ret = R_NEW0 (OMF_multi_datas)))
		return R_FALSE;
	record->content = ret;

	while (tmp_size < record->size - 1) {
		ret->nb_elem++;
		tmp_size += buf[3 + tmp_size] + 1;
	}
	if (!(ret->elems = R_NEWS0 (char *, ret->nb_elem))) {
		R_FREE(ret);
		return R_FALSE;
	}
	names = (char **)ret->elems;
	
	tmp_size = 0;
	while (tmp_size < record->size - 1) {
		// sometimes there is a name with a null size so we just skip it
		if (!buf[3 + tmp_size]) {
			names[ct_name++] = NULL;
			tmp_size++;
			continue;
		}

		if (record->size + 3 < buf[3 + tmp_size] + tmp_size) {
			eprintf ("Invalid Lnames record (bad size)\n");
			return R_FALSE;
		}

		if (!(names[ct_name] = R_NEWS0 (char, buf[3 + tmp_size] + 1))) {
			free_lname (ret);
			return R_FALSE;
		}
      
		memcpy (names[ct_name], buf + 3 + tmp_size + 1,
			buf[3 + tmp_size]);

		ct_name++;
		tmp_size += buf[3 + tmp_size] + 1;
	}
	return R_TRUE;
}

static int load_omf_segdef(OMF_record *record, const char *buf, ut64 buf_size) {
	OMF_segment *ret = NULL;
	int off_add;
	
	if (!(ret = R_NEW0 (OMF_segment)))
		return R_FALSE;
	record->content = ret;

	if (record->size < 2) {
		eprintf ("Invalid Segdef record (bad size)\n");
		return R_FALSE;
	}
	off_add = buf[3] & 0xe ? 0 : 3;


	if (record->type == OMF_SEGDEF32) {
		if (record->size < 5 + off_add) {
			eprintf ("Invalid Segdef record (bad size)\n");
			return R_FALSE;
		}
		ret->name_idx = omf_get_idx (buf + 8 + off_add);
		if (buf[3] & 2)
			ret->size = UT32_MAX;
		else ret->size = *((ut32 *)(buf + 4 + off_add));
	} else {
		if (record->size < 3 + off_add) {
			eprintf ("Invalid Segdef record (bad size)\n");
			return R_FALSE;
		}
		ret->name_idx = omf_get_idx (buf + 6 + off_add);
		if (buf[3] & 2)
			ret->size = UT16_MAX;
		ret->size = *((ut16 *)(buf + 4 + off_add));
	}
  
	if (buf[3] & 1)
		ret->bits = 32;
	else ret->bits = 16;

	// tricks to keep the save index when copying content from record
	record->type = OMF_SEGDEF;

	return R_TRUE;
}

static ut32 omf_count_symb(ut16 total_size, ut32 ct, const char *buf, int bits) {
	ut32 nb_symb = 0;

	while (ct < total_size - 1) {
		ct += buf[ct] + 1 + (bits == 32 ? 4 : 2);
		if (ct > total_size - 1)
			return nb_symb;
		if (buf[ct] & 0x80)
			ct += 2;
		else ct++;
		nb_symb++;
	}
	return nb_symb;
}

static int load_omf_symb(OMF_record *record, ut32 ct, const char *buf, int bits, ut16 seg_idx) {
	ut32 nb_symb = 0;
	ut8 str_size = 0;
	OMF_symbol *symbol;
	
	while (nb_symb < ((OMF_multi_datas *)record->content)->nb_elem) {
		symbol = ((OMF_symbol *)((OMF_multi_datas *)record->content)->elems) + nb_symb;
	  
		if (record->size - 1 < ct - 2) {
			eprintf ("Invalid Pubdef record (bad size)\n");
			return R_FALSE;
		}
		str_size = buf[ct];
		
		if (bits == 32) {
			if (ct + 1 + str_size + 4 - 3 > record->size) {
				eprintf ("Invalid Pubdef record (bad size)\n");
				return R_FALSE;
			}
			symbol->offset = *((ut32 *)(buf + ct + 1 + str_size));
		} else {
			if (ct + 1 + str_size + 2 - 3 > record->size) {
				eprintf ("Invalid Pubdef record (bad size)\n");
				return R_FALSE;
			}
			symbol->offset = *((ut16 *)(buf + ct + 1 + str_size));
		}

		symbol->seg_idx = seg_idx;
		
		if (!(symbol->name = R_NEWS0 (char, str_size + 1)))
			return R_FALSE;
		symbol->name[str_size] = 0;
		memcpy (symbol->name, buf + ct + 1, sizeof(char) * str_size);
		
		ct += 1 + str_size + (bits == 32 ? 4 : 2);
		if (buf[ct] & 0x80) //type index
			ct += 2;
		else ct++;
		nb_symb++;
	}
	return R_TRUE;
}

static int load_omf_pubdef(OMF_record *record, const char *buf) {
	OMF_multi_datas *ret = NULL;
	ut16 seg_idx;
	ut16 ct = 0;
	ut16 base_grp;
	
	if (record->size < 2) {
		eprintf ("Invalid Pubdef record (bad size)\n");
		return R_FALSE;
	}

	ct = 3;
	base_grp = omf_get_idx (buf + ct);
	if (buf[ct] & 0x80) // sizeof base groups index
		ct += 2;
	else ct++;
  
	if (record->size < ct - 2) {
		eprintf ("Invalid Pubdef record (bad size)\n");
		return R_FALSE;
	}
	
	seg_idx = omf_get_idx (buf + ct);
	
	if (buf[ct] & 0x80) // sizeof base segment index
		ct += 2;
	else ct++;

	if (!base_grp && !seg_idx)
		ct += 2;
	if (record->size < ct - 2) {
		eprintf ("Invalid Pubdef record (bad size)\n");
		return R_FALSE;
	}

	if(!(ret = R_NEW0 (OMF_multi_datas)))
		return R_FALSE;
	record->content = ret;

	if (!(record->type & 1)) { // 16 bit addr
		ret->nb_elem = omf_count_symb (record->size + 3, ct, buf, 16);
		if (!(ret->elems = R_NEWS0 (OMF_symbol, ret->nb_elem)))
			return R_FALSE;
		if (!load_omf_symb (record, ct, buf, 16, seg_idx))
			return R_FALSE;
	}
	else { // 32 bit addr
		ret->nb_elem = omf_count_symb (record->size + 3, ct, buf, 32);
		if (!(ret->elems = R_NEWS0 (OMF_symbol, ret->nb_elem)))
			return R_FALSE;
		if (!load_omf_symb (record, ct, buf, 32, seg_idx))
			return R_FALSE;
	}

	// tricks to keep the save index when copying content from record
	record->type = OMF_PUBDEF;
	return R_TRUE;
}

static int load_omf_data(const char *buf, OMF_record *record, ut64 global_ct) {
	ut16 seg_idx;
	ut32 offset;
	ut16 ct = 4;
	OMF_data *ret;

	if ((!(record->type & 1) && record->size < 4) || (record->size < 6)) {
		eprintf ("Invalid Ledata record (bad size)\n");
		return R_FALSE;
	}
	seg_idx = omf_get_idx (buf + 3);
	if (seg_idx & 0xff00) {
		if ((!(record->type & 1) && record->size < 5) || (record->size < 7)) {
			eprintf ("Invalid Ledata record (bad size)\n");
			return R_FALSE;
		}
		ct++;
	}
	if (record->type == OMF_LEDATA32) {
		offset = *((ut32 *)(buf + ct));
		ct += 4;
	} else {
		offset = *((ut16 *)(buf + ct));
		ct += 2;
	}
	if (!(ret = R_NEW0 (OMF_data)))
		return R_FALSE;
	record->content = ret;

	ret->size = record->size - 1 - (ct - 3);
	ret->paddr = global_ct + ct;
	ret->offset = offset;
	ret->seg_idx = seg_idx;
	ret->next = NULL;
	record->type = OMF_LEDATA;

	return R_TRUE;
}


static int load_omf_content(OMF_record *record, const char *buf, ut64 global_ct, ut64 buf_size) {
	if (record->type == OMF_LNAMES)
		return load_omf_lnames (record, buf, buf_size);
	else if (record->type == OMF_SEGDEF || record->type == OMF_SEGDEF32)
		return load_omf_segdef (record, buf, buf_size);
	else if (record->type == OMF_PUBDEF || record->type == OMF_PUBDEF32 || record->type == OMF_LPUBDEF || record->type == OMF_LPUBDEF32)
		return load_omf_pubdef (record, buf);
	else if (record->type == OMF_LEDATA || record->type == OMF_LEDATA32)
		return load_omf_data (buf, record, global_ct);

	// generic loader just copy data from buf to content
	if (!record->size) {
		eprintf("Invalid record (size to short)\n");
		return R_FALSE;
	}
	if (!(record->content = R_NEWS0 (char, record->size)))
		return R_FALSE;
	((char *)record->content)[record->size - 1] = 0;
	return R_TRUE;
}

static OMF_record_handler *load_record_omf(const char *buf, ut64 global_ct, ut64 buf_size){
	OMF_record_handler *new = NULL;

	if (is_valid_omf_type (*buf) && r_bin_checksum_omf_ok (buf, buf_size)) {
		if (!(new = R_NEW0 (OMF_record_handler)))
			return NULL;
		((OMF_record *)new)->type = *buf;
		((OMF_record *)new)->size = *((ut16 *)(buf + 1));
		
		// at least a record have a type a size and a checksum
		if (((OMF_record *)new)->size > buf_size - 3 || buf_size < 4) {
		  eprintf("Invalid record (too short)\n");
		  R_FREE(new);
		  return NULL;
		}

		if (!(load_omf_content ((OMF_record *)new, buf, global_ct, buf_size))) {
		  R_FREE(new);
		  return NULL;
		}
		((OMF_record *)new)->checksum = buf[2 + ((OMF_record *)new)->size];
		new->next = NULL;
	}
	return new;
}

static int load_all_omf_records(r_bin_omf_obj *obj, const char *buf, ut64 size) {
	ut64 ct = 0;
	OMF_record_handler *new_rec = NULL;
	OMF_record_handler *tmp = NULL;
	
	while (ct < size) {
		if (!(new_rec = load_record_omf (buf + ct, ct, size - ct)))
			return R_FALSE;

		// the order is important because some link are made by index
		if (!tmp) {
			obj->records = new_rec;
			tmp = obj->records;
		} else {
			tmp->next = new_rec;
			tmp = tmp->next;
		}
		ct += 3 + ((OMF_record *)tmp)->size;
	}
	return R_TRUE;
}

static ut32 count_omf_record_type(r_bin_omf_obj *obj, ut8 type) {
	OMF_record_handler *tmp = obj->records;
	ut32 ct = 0;

	while (tmp) {
		if (((OMF_record *)tmp)->type == type)
			ct++;
		tmp = tmp->next;
	}
	return ct;
}

static ut32 count_omf_multi_record_type(r_bin_omf_obj *obj, ut8 type) {
	OMF_record_handler *tmp = obj->records;
	ut32 ct = 0;

	while (tmp) {
		if (((OMF_record *)tmp)->type == type)
			ct += ((OMF_multi_datas*)((OMF_record *)tmp)->content)->nb_elem;
		tmp = tmp->next;
	}
	return ct;
}

static OMF_record_handler *get_next_omf_record_type(OMF_record_handler *tmp, ut8 type) {
	while (tmp) {
		if (((OMF_record *)tmp)->type == type)
			return (tmp);
		tmp = tmp->next;
	}
	return NULL;
}

static int cpy_omf_names(r_bin_omf_obj *obj) {
	OMF_record_handler *tmp = obj->records;
	OMF_multi_datas	*lname;
	int ct_obj = 0;
	int ct_rec;

	while ((tmp = get_next_omf_record_type(tmp, OMF_LNAMES))) {
		lname = (OMF_multi_datas *)((OMF_record *)tmp)->content;

		ct_rec = -1;
		while (++ct_rec < lname->nb_elem) {
			if (!((char **)lname->elems)[ct_rec])
				obj->names[ct_obj++] = NULL;
			else if (!(obj->names[ct_obj++] = strdup(((char **)lname->elems)[ct_rec])))
				return R_FALSE;
		}
		tmp = tmp->next;
	}
	return R_TRUE;
}

static void get_omf_section_info(r_bin_omf_obj *obj) {
	OMF_record_handler *tmp = obj->records;
	ut32 ct_obj = 0;
	
	while ((tmp = get_next_omf_record_type (tmp, OMF_SEGDEF))) {
		obj->sections[ct_obj] = ((OMF_record *)tmp)->content;
		((OMF_record *)tmp)->content = NULL;

		if (!ct_obj)
			obj->sections[ct_obj]->vaddr = 0;
		else obj->sections[ct_obj]->vaddr =  obj->sections[ct_obj - 1]->vaddr +  obj->sections[ct_obj - 1]->size;
		ct_obj++;
		tmp = tmp->next;
	 }
}

static int get_omf_symbol_info(r_bin_omf_obj *obj) {
	OMF_record_handler	*tmp = obj->records;
	OMF_multi_datas	*symbols;
	int ct_obj = 0;
	int			ct_rec = 0;

	while ((tmp = get_next_omf_record_type(tmp, OMF_PUBDEF))) {
		symbols = (OMF_multi_datas *)((OMF_record *)tmp)->content;

		ct_rec = -1;
		while (++ct_rec < symbols->nb_elem) {
			if(!(obj->symbols[ct_obj] = R_NEW0(OMF_symbol)))
				return R_FALSE;
			memcpy(obj->symbols[ct_obj], ((OMF_symbol *)symbols->elems) + ct_rec, sizeof(*(obj->symbols[ct_obj])));
			obj->symbols[ct_obj]->name = strdup(((OMF_symbol *)symbols->elems)[ct_rec].name);
			ct_obj++;
		 }
		tmp = tmp->next;
	 }
	return R_TRUE;
}

static int get_omf_data_info(r_bin_omf_obj *obj) {
	OMF_record_handler *tmp = obj->records;
	OMF_data *tmp_data;

	while ((tmp = get_next_omf_record_type (tmp, OMF_LEDATA))) {
		if (((OMF_data *)((OMF_record *)tmp)->content)->seg_idx - 1 >= obj->nb_section) {
			eprintf ("Invalid Ledata record (bad segment index)\n");
			return R_FALSE;
		}
		if (!(tmp_data = obj->sections[((OMF_data *)((OMF_record *)tmp)->content)->seg_idx - 1]->data))
			obj->sections[((OMF_data *)((OMF_record *)tmp)->content)->seg_idx - 1]->data = ((OMF_record *)tmp)->content;
		else {
			while (tmp_data->next)
				tmp_data = tmp_data->next;

			tmp_data->next = ((OMF_record *)tmp)->content;
		}
		((OMF_record *)tmp)->content = NULL;
		tmp = tmp->next;
	}
	return R_TRUE;
}

static int get_omf_infos(r_bin_omf_obj *obj) {
	// get all name defined in lnames records
	obj->nb_name = count_omf_multi_record_type (obj, OMF_LNAMES);
	if (!(obj->names = R_NEWS0 (char *, obj->nb_name)))
		return R_FALSE;
	if (!cpy_omf_names (obj))
		return R_FALSE;

	// get all sections (segdef record)
	obj->nb_section = count_omf_record_type (obj, OMF_SEGDEF);
	if (!(obj->sections = R_NEWS0 (OMF_segment *, obj->nb_section)))
		return R_FALSE;
	get_omf_section_info (obj);
	
	// get all data (ledata record)
	get_omf_data_info (obj);

	// get all symbols (pubdef + lpubdef)
	obj->nb_symbol = count_omf_multi_record_type (obj, OMF_PUBDEF);
	if (!(obj->symbols = R_NEWS0 (OMF_symbol *, obj->nb_symbol)))
		return R_FALSE;
	if (!get_omf_symbol_info (obj))
		return R_FALSE;

	return R_TRUE;
}

static void free_pubdef(OMF_multi_datas *datas) {
	ut32 ct_rec = 0;

	while (ct_rec < datas->nb_elem)
		R_FREE (((OMF_symbol *)(datas->elems + ct_rec++))->name);

	R_FREE (datas->elems);
	R_FREE (datas);
}

static void free_all_omf_records(r_bin_omf_obj *obj) {
	OMF_record_handler *tmp = NULL;
	OMF_record_handler *rec = obj->records;

	while (rec) {
		if (((OMF_record *)rec)->type == OMF_LNAMES)
			free_lname((OMF_multi_datas *)((OMF_record *)rec)->content);
		else if (((OMF_record *)rec)->type == OMF_PUBDEF)
			free_pubdef((OMF_multi_datas *)((OMF_record *)rec)->content);
		else R_FREE (((OMF_record *)rec)->content);
		tmp = rec->next;
		R_FREE(rec);
		rec = tmp;
	}
	obj->records = NULL;
}

static void free_all_omf_sections(r_bin_omf_obj *obj) {
	ut32 ct = 0;
	OMF_data *data;

	while (ct < obj->nb_section) {
		while (obj->sections[ct]->data) {
			data = obj->sections[ct]->data->next;
			R_FREE(obj->sections[ct]->data);
			obj->sections[ct]->data = data;
		}
		R_FREE(obj->sections[ct]);
		ct++;
	}
	R_FREE (obj->sections);
}

static void free_all_omf_symbols(r_bin_omf_obj *obj) {
	ut32 ct = 0;
	while (ct < obj->nb_symbol) {
		R_FREE(obj->symbols[ct]->name);
		R_FREE(obj->symbols[ct]);

		ct++;
	}
	R_FREE (obj->symbols);
}

static void free_all_omf_names(r_bin_omf_obj *obj) {
	ut32 ct = 0;

	while (ct < obj->nb_name) {
		R_FREE (obj->names[ct]);
		ct++;
	}
	R_FREE(obj->names);
}

void r_bin_free_all_omf_obj(r_bin_omf_obj *obj) {
	if (obj->records)
		free_all_omf_records(obj);
	if (obj->sections)
		free_all_omf_sections(obj);
	if (obj->symbols)
		free_all_omf_symbols(obj);
	if (obj->names)
		free_all_omf_names(obj);
	R_FREE(obj);
}

r_bin_omf_obj *r_bin_internal_omf_load(const char *buf, ut64 size) {
	r_bin_omf_obj *ret = NULL;

	if (!(ret = R_NEW0 (r_bin_omf_obj)))
		return NULL;

	if (!load_all_omf_records(ret, buf, size)) {
		r_bin_free_all_omf_obj(ret);
		return NULL;
	}
	if(!(get_omf_infos(ret))) {
		r_bin_free_all_omf_obj(ret);
		return NULL;
	}
	free_all_omf_records(ret);
	return ret;
}

int r_bin_omf_get_entry(r_bin_omf_obj *obj, RBinAddr *addr) {
	ut32 ct_sym = 0;
	OMF_data *data;
	ut32 offset = 0;
	
	while (ct_sym < obj->nb_symbol) {
		if (!strcmp (obj->symbols[ct_sym]->name, "_start")) {
			if (obj->symbols[ct_sym]->seg_idx - 1 > obj->nb_section) {
				eprintf ("Invalid segment index for symbol _start\n");
				return R_FALSE;
			}			
			addr->vaddr = obj->sections[obj->symbols[ct_sym]->seg_idx - 1]->vaddr + obj->symbols[ct_sym]->offset + OMF_BASE_ADDR;
			data = obj->sections[obj->symbols[ct_sym]->seg_idx - 1]->data;
			while (data) {
				offset += data->size;
				if (obj->symbols[ct_sym]->offset < offset) {
					addr->paddr = (obj->symbols[ct_sym]->offset - data->offset) + data->paddr;
					return R_TRUE;
				}
				data = data->next;
			}
		}
		ct_sym++;
	}
	return R_FALSE;
}

int r_bin_omf_get_bits(r_bin_omf_obj *obj) {
	ut32 ct_sec = 0;

	// we assume if one segdef define a 32 segment all opcodes are 32bits
	while (ct_sec < obj->nb_section)
		if (obj->sections[ct_sec++]->bits == 32)
			return 32;
	return 16;
}

int r_bin_omf_send_sections(RList *list, OMF_segment *section, r_bin_omf_obj *obj) {
	RBinSection *new;
	OMF_data *data = section->data;
	ut32 ct_name = 1;
  
	while (data) {
		if (!(new = R_NEW0 (RBinSection)))
			return R_FALSE;

		// if index == 0, it's mean there is no name
		if (section->name_idx && section->name_idx - 1 < obj->nb_name)
			snprintf (new->name, R_BIN_SIZEOF_STRINGS, "%s_%d", obj->names[section->name_idx - 1], ct_name++);
		else snprintf (new->name, R_BIN_SIZEOF_STRINGS, "no_name_%d", ct_name++);

		new->size = data->size;
		new->vsize = data->size;
		new->paddr = data->paddr;
		new->vaddr = section->vaddr + data->offset + OMF_BASE_ADDR;
		new->srwx = R_BIN_SCN_EXECUTABLE | R_BIN_SCN_WRITABLE | R_BIN_SCN_READABLE;
		r_list_append (list, new);
		data = data->next;
	}
	return R_TRUE;
}

ut64 r_bin_omf_get_paddr_sym(r_bin_omf_obj *obj, OMF_symbol *sym) {
	OMF_data *data;
	ut64 offset = 0;
	
	if (sym->seg_idx - 1 > obj->nb_section)
		return 0;

	data = obj->sections[sym->seg_idx - 1]->data;
	while (data) {
		offset += data->size;
		if (sym->offset < offset)
			return sym->offset - data->offset + data->paddr;
	  data = data->next;
	}
	return 0;
}

ut64 r_bin_omf_get_vaddr_sym(r_bin_omf_obj *obj, OMF_symbol *sym) {
	if (sym->seg_idx - 1 > obj->nb_section) {
		eprintf ("Invalid segment index for symbol %s\n", sym->name);
		return 0;
	}
	return obj->sections[sym->seg_idx - 1]->vaddr + sym->offset + OMF_BASE_ADDR;
}
