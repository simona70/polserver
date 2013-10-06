/*
History
=======
2005/01/23 Shinigami: for_all_mobiles, write_items & write_multis - Tokuno MapDimension doesn't fit blocks of 64x64 (WGRID_SIZE)
2007/06/17 Shinigami: added config.world_data_path
2009/09/03 MuadDib:   Relocation of account related cpp/h
                      Relocation of multi related cpp/h
2009/09/14 MuadDib:   All UOX3 Import Code commented out. You can script this.
2009/12/02 Turley:    added config.max_tile_id - Tomi
2011/11/28 MuadDib:   Removed last of uox referencing code.

Notes
=======

*/

#include "../clib/stl_inc.h"
#ifdef _MSC_VER
#pragma warning( disable: 4786 )
#endif


#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "../clib/cfgelem.h"
#include "../clib/cfgfile.h"
#include "../clib/endian.h"
#include "../clib/esignal.h"
#include "../clib/fileutil.h"
#include "../clib/logfile.h"
#include "../clib/progver.h"
#include "../clib/stlutil.h"
#include "../clib/strutil.h"
#include "../clib/timer.h"

#include "../plib/polver.h"
#include "../plib/realm.h"

#include "accounts/account.h"
#include "accounts/accounts.h"
#include "mobile/charactr.h"
#include "fnsearch.h"
#include "gflag.h"
#include "gprops.h"
#include "item/itemdesc.h"
#include "loaddata.h"
#include "objtype.h"
#include "npc.h"
#include "polcfg.h"
#include "polvar.h"
#include "realms.h"
#include "resource.h"
#include "savedata.h"
#include "servdesc.h"
#include "sockio.h"
#include "startloc.h"
#include "storage.h"
#include "stubdata.h"
#include "uvars.h"
#include "ufunc.h"
#include "uworld.h"
#include "multi/multi.h"

////HASH
#include "objecthash.h"
////

typedef std::vector<Item*> ContItemArr;
ContItemArr contained_items;

typedef std::vector<unsigned int> ContSerArr;
ContSerArr  container_serials;

/****************** POL Native Files *******************/
//Dave changed 3/8/3 to use objecthash
void read_character( ConfigElem& elem )
{
	// if this object is modified in a subsequent incremental save,
	// don't load it now. 
	pol_serial_t serial = 0;
	elem.get_prop( "SERIAL", &serial );
	if (get_save_index( serial ) > current_incremental_save)
		return;

	CharacterRef chr( new Character( elem.remove_ushort( "OBJTYPE" ) ) );

	try
	{
		// note chr->logged_in is true..
		chr->readProperties( elem );
		chr->clear_dirty();
		
		// readProperties gets the serial, so we can't add to the objecthash until now.
		objecthash.Insert(chr.get());
	}
	catch( exception& )
	{
		if (chr.get() != NULL)
			chr->destroy();
		throw;
	}
}

//Dave changed 3/8/3 to use objecthash
void read_npc( ConfigElem& elem )
{
	// if this object is modified in a subsequent incremental save,
	// don't load it now. 
	pol_serial_t serial = 0;
	elem.get_prop( "SERIAL", &serial );
	if (get_save_index( serial ) > current_incremental_save)
		return;

	NPCRef npc( new NPC( elem.remove_ushort( "OBJTYPE" ), elem ) );

	try
	{
		npc->readProperties( elem );

		SetCharacterWorldPosition( npc.get() );
		npc->clear_dirty();

		////HASH
		objecthash.Insert(npc.get());
		////
	}
	catch( exception& )
	{
		if (npc.get() != NULL)
			npc->destroy();
		throw;
	}
}

// hmm, an interesting idea, what if there was an
// Item::create( ConfigElem& elem ) member,
// which would do this?
// Item would need a constructor (ConfigElem&) also.
// Polymorphism would then take
// care of it, and move the logic into the derived
// classes.
Item* read_item( ConfigElem& elem )
{
	u32 serial;
	u32 objtype;
	if (elem.remove_prop( "SERIAL", &serial ) == false)
	{
		cerr << "Item element has no SERIAL property, omitting." << endl;
		return NULL;
	}

	if (config.check_integrity)
	{
		if (system_find_item( serial ))
		{
			cerr << "Duplicate item read from datafiles (Serial=0x"
				  << hex << serial << dec << ")"
				  << endl;
			throw runtime_error( "Data integrity error" );
		}
	}
	if (elem.remove_prop( "OBJTYPE", &objtype ) == false)
	{
		cerr << "Item (Serial 0x" << hex << serial << dec << ") has no OBJTYPE property, omitting." << endl;
		return NULL;
	}
	if (old_objtype_conversions.count( objtype ))
		objtype = old_objtype_conversions[ objtype ];

	Item* item = Item::create( objtype, serial );
	if (item == NULL)
	{
		cerr << "Unable to create item: objtype=" << hexint(objtype) << ", serial=0x" << hex << serial << dec << endl;
		if ( !config.ignore_load_errors )
			throw runtime_error( "Item::create failed!" );
		else
			return NULL;
	}
	item->realm = find_realm(string("britannia"));
		
	item->readProperties( elem );

	item->clear_dirty();

	return item;
}

#define USE_PARENT_CONTS 1

typedef std::stack<UContainer*> ContStack;
static ContStack parent_conts;

void read_global_item( ConfigElem& elem, int sysfind_flags )
{
	// if this object is modified in a subsequent incremental save,
	// don't load it now. 
	pol_serial_t serial = 0;
	elem.get_prop( "SERIAL", &serial );
	if (get_save_index( serial ) > current_incremental_save)
		return;


	u32 container_serial = 0;
 	elem.remove_prop( "CONTAINER", &container_serial );

	Item* item = read_item( elem );
	//dave added 1/15/3, protect against further crash if item is null. Should throw instead?
	if (item == NULL)
	{
		elem.warn_with_line("Error reading item SERIAL or OBJTYPE.");
		return;
	}

	ItemRef itemref(item); //dave 1/28/3 prevent item from being destroyed before function ends
	if (container_serial == 0)
	{
		add_item_to_world( item );
		if (item->isa( UObject::CLASS_CONTAINER ))
			parent_conts.push( static_cast<UContainer*>(item) );
	}
	else
	{
		if (IsCharacter( container_serial )) // it's equipped on a character
		{
			Character* chr = system_find_mobile( container_serial );
			if (chr != NULL)
			{
				equip_loaded_item( chr, item );
			}
			else
			{
				defer_item_insertion( item, container_serial );
			}
			return;
		}
		Item* cont_item = NULL;
		//bool new_parent_cont = false;

		while (!parent_conts.empty())
		{
			UContainer* cont = parent_conts.top();
			if (cont->serial == container_serial)
			{
				cont_item = cont;
				break;
			}
			else
			{
				parent_conts.pop();
			}
		}

		if (cont_item == NULL)
		{
			cont_item = system_find_item( container_serial );
			//new_parent_cont = true;
		}

		if (cont_item)
		{
			add_loaded_item( cont_item, item );
		}
		else
		{
			defer_item_insertion( item, container_serial );
		}
	}
}

void read_system_vars(ConfigElem& elem)
{
	polvar.DataWrittenBy = elem.remove_ushort( "CoreVersion" );
	stored_last_item_serial = elem.remove_ulong( "LastItemSerialNumber", UINT_MAX ); //dave 3/9/3
	stored_last_char_serial = elem.remove_ulong( "LastCharSerialNumber", UINT_MAX ); //dave 3/9/3
}

void read_shadow_realms(ConfigElem& elem)
{
	std::string name = elem.remove_string("Name");
	Realm* baserealm = find_realm(elem.remove_string("BaseRealm"));
	if ( !baserealm )
		elem.warn_with_line("BaseRealm not found.");
	if ( defined_realm(name) )
		elem.warn_with_line("Realmname already defined");
	add_realm(name, baserealm);
	cout << endl << "Shadowrealm " << name;
}

void read_multi( ConfigElem& elem )
{
	// if this object is modified in a subsequent incremental save,
	// don't load it now. 
	pol_serial_t serial = 0;
	elem.get_prop( "SERIAL", &serial );
	if (get_save_index( serial ) > current_incremental_save)
		return;

	u32 objtype;
	if (elem.remove_prop( "SERIAL", &serial ) == false)
	{
		cerr << "A Multi has no SERIAL property." << endl;
		throw runtime_error( "Config File error." );
	}
	if (system_find_multi( serial ) || system_find_item( serial ))
	{
		cerr << "Duplicate item read from datafiles (Serial=0x"
			  << hex << serial << dec << ")"
			  << endl;
		throw runtime_error( "Data integrity error" );
	}
	if (elem.remove_prop( "OBJTYPE", &objtype ) == false)
	{
		cerr << "Multi (Serial 0x" << hex << serial << dec << ") has no OBJTYPE property, omitting." << endl;
		return;
	}
	if (old_objtype_conversions.count( objtype ))
		objtype = old_objtype_conversions[ objtype ];

	UMulti* multi = UMulti::create( find_itemdesc(objtype), serial );
	if (multi == NULL)
	{
		cerr << "Unable to create multi: objtype=" << hexint(objtype) << ", serial=" << hexint(serial) << endl;
		throw runtime_error( "Multi::create failed!" );
	}
	multi->readProperties( elem );

	add_multi_to_world( multi );
}

string elapsed( clock_t start, clock_t end )
{
	size_t ms = static_cast<size_t>((end-start) * 1000.0 / CLOCKS_PER_SEC);
	return decint( ms ) + " ms";
}

void slurp( const char* filename, const char* tags, int sysfind_flags )
{
	static int num_until_dot = 1000;
	
	if (FileExists( filename ))
	{
		cout << "  " << filename << ":";
		ConfigFile cf( filename, tags );
		ConfigElem elem;

		Tools::Timer<> timer;
		
		unsigned int nobjects = 0;
		while (cf.read( elem ))
		{
			if (--num_until_dot == 0)
			{
				cout << ".";
				num_until_dot = 1000;
			}
			try {
				if (stricmp( elem.type(), "CHARACTER" ) == 0)
					read_character( elem );
				else if (stricmp( elem.type(), "NPC" ) == 0)
					read_npc( elem );
				else if (stricmp( elem.type(), "ITEM" ) == 0)
					read_global_item( elem, sysfind_flags );
				else if (stricmp( elem.type(), "GLOBALPROPERTIES" ) == 0)
					global_properties.readProperties( elem );
				else if (elem.type_is( "SYSTEM" ))
					read_system_vars( elem );
				else if (elem.type_is( "MULTI" ))
					read_multi( elem );
				else if (elem.type_is( "STORAGEAREA" ))
				{
					StorageArea* storage_area = storage.create_area( elem );
					// this will be followed by an item
					cf.read( elem );
					storage_area->load_item( elem );
				}
				else if (elem.type_is( "REALM" ))
					read_shadow_realms( elem );

			}
			catch( std::exception& )
			{
				if (!config.ignore_load_errors)
					throw;
			}
			++nobjects;
		}

		timer.stop();

		cout << " " << nobjects << " elements in " << timer.ellapsed() << " ms." << endl;
	}
}

void read_pol_dat()
{
	string polfile = config.world_data_path + "pol.txt";

	slurp( polfile.c_str(), "GLOBALPROPERTIES SYSTEM REALM" );

	if (polvar.DataWrittenBy == 0)
	{
		cerr << "CoreVersion not found in " << polfile << endl << endl;
		cerr << polfile << " must contain a section similar to: " << endl;
		cerr << "System" << endl
			 << "{" << endl
			 << "	CoreVersion 93" << endl
			 << "}" << endl
			 << endl;
		cerr << "Ensure that the CoreVersion matches the version that created your data files!" << endl;
		throw runtime_error( "Data file error" );
	}
}

void read_objects_dat()
{
	slurp( (config.world_data_path + "objects.txt").c_str(), "CHARACTER NPC ITEM GLOBALPROPERTIES" );
}

void read_pcs_dat()
{
	slurp( (config.world_data_path + "pcs.txt").c_str(), "CHARACTER ITEM", SYSFIND_SKIP_WORLD );
}

void read_pcequip_dat()
{
	slurp( (config.world_data_path + "pcequip.txt").c_str(), "ITEM", SYSFIND_SKIP_WORLD );
}

void read_npcs_dat()
{
	slurp( (config.world_data_path + "npcs.txt").c_str(), "NPC ITEM", SYSFIND_SKIP_WORLD );
}

void read_npcequip_dat()
{
	slurp( (config.world_data_path + "npcequip.txt").c_str(), "ITEM", SYSFIND_SKIP_WORLD );
}
	
void read_items_dat()
{
	slurp( (config.world_data_path + "items.txt").c_str(), "ITEM" );
}

void read_multis_dat()
{
	slurp( (config.world_data_path + "multis.txt").c_str(), "MULTI" );
//	string multisfile = config.world_data_path + "multis.txt";
//	if (FileExists( multisfile ))
//	{
//		cout << multisfile << ":";
//		ConfigFile cf( multisfile, "MULTI" );
//		ConfigElem elem;
//		while( cf.read( elem ))
//		{
//			UMulti* multi = read_multi( elem );
//			if (multi == NULL) throw runtime_error( "multi creation returned NULL!" );
//
//			add_multi_to_world( multi );
//		}
//	}
}

void read_storage_dat()
{
	string storagefile = config.world_data_path + "storage.txt";

	if (FileExists( storagefile ))
	{
		cout << "  " << storagefile << ":";
		ConfigFile cf2( storagefile );
		storage.read( cf2 );
	}
}

Item* find_existing_item( u32 objtype, u16 x, u16 y, s8 z, Realm* realm )
{
	unsigned short wx, wy;
	zone_convert( x, y, wx, wy, realm );
	ZoneItems& witem = realm->zone[wx][wy].items;
	for( ZoneItems::iterator itr = witem.begin(), end = witem.end(); itr != end; ++itr )
	{
		Item* item = *itr;

				// FIXME won't find doors which have been perturbed
		if (item->objtype_ == objtype &&
			item->x == x &&
			item->y == y &&
			item->z == z)
		{
			return item;
		}
	}
	return NULL;
}

int import_count;
int dupe_count;

void import( ConfigElem& elem )
{
	u32 objtype;
	objtype = elem.remove_unsigned( "OBJTYPE" );
	if (objtype > config.max_tile_id)
	{
		cerr << "Importing file: " << hex << objtype << dec << " is out of range." << endl;
		throw runtime_error( "Error while importing file." );
	}

	Item* item = Item::create( objtype, 0x40000000 ); // dummy serial
	
	if (item == NULL)
	{
		cerr << "Unable to import item: objtype=" << objtype << endl;
		throw runtime_error( "Item::create failed!" );
	}
	
	item->readProperties( elem );
	
	if (find_existing_item( item->objtype_, item->x, item->y, item->z, item->realm ))
	{
		item->destroy();
		++dupe_count;
	}
	else
	{
		item->serial = GetNewItemSerialNumber();
		
		item->serial_ext = ctBEu32( item->serial );

		add_item_to_world(item);
		register_with_supporting_multi( item );
		++import_count;
	}

}

void import_new_data()
{
	string importfile = config.world_data_path + "import.txt";

	if (FileExists( importfile ))
	{
		ConfigFile cf( importfile, "ITEM" );
		ConfigElem elem;
		while (cf.read( elem ))
		{
			import( elem );
		}
		unlink( importfile.c_str() );
		cout << "Import Results: " << import_count << " imported, " << dupe_count << " duplicates." << endl;
	}
}

void rndat( const string& basename )
{
	string datname = config.world_data_path + basename + ".dat";
	string txtname = config.world_data_path + basename + ".txt";

	if (FileExists( datname.c_str() ))
	{
		rename( datname.c_str(), txtname.c_str() );
	}
}

void rename_dat_files()
{
	rndat( "pol" );
	rndat( "objects" );
	rndat( "pcs" );
	rndat( "pcequip" );
	rndat( "npcs" );
	rndat( "npcequip" );
	rndat( "items" );
	rndat( "multis" );
	rndat( "storage" );
	rndat( "resource" );
	rndat( "guilds" );
	rndat( "parties" );
}

void read_guilds_dat();
void write_guilds( StreamWriter& sw );

void read_datastore_dat();
void write_datastore( StreamWriter& sw );
void commit_datastore();

void read_party_dat();
void write_party( StreamWriter& sw );

void for_all_mobiles( void (*f)(Character* chr) )
{
	for( const auto &realm : *Realms )
	{
		unsigned wgridx = realm->width() / WGRID_SIZE;
		unsigned wgridy = realm->height() / WGRID_SIZE;

	// Tokuno-Fix
	if (wgridx * WGRID_SIZE < realm->width())
	  wgridx++;
	if (wgridy * WGRID_SIZE < realm->height())
	  wgridy++;
	
		for( unsigned wx = 0; wx < wgridx; ++wx )
		{
			for( unsigned wy = 0; wy < wgridy; ++wy )
			{
				for( auto &z_chr : realm->zone[wx][wy].characters )
				{
					(*f)(z_chr);
				}
			}
		}
	}
}

int read_data()
{
	string objectsndtfile = config.world_data_path + "objects.ndt";
	string storagendtfile = config.world_data_path + "storage.ndt";

	gflag_in_system_load = true;
	if (FileExists( objectsndtfile ))
	{
		// Display reads "Reading data files..."
		cerr << "Error!" << endl
			 << "'" << objectsndtfile << " exists.  This probably means the system" 
			 << endl
			 << "exited while writing its state.  To avoid loss of data," 
			 << endl
			 << "forcing human intervention."
			 << endl;
		throw runtime_error( "Human intervention required." );
	}
	if (FileExists( storagendtfile ))
	{
		cerr << "Error!" << endl
			 << "'" << storagendtfile << " exists.  This probably means the system" 
			 << endl
			 << "exited while writing its state.  To avoid loss of data," 
			 << endl
			 << "forcing human intervention."
			 << endl;
		throw runtime_error( "Human intervention required." );
	}

	rename_dat_files();

	load_incremental_indexes();

	read_pol_dat();

			// POL clock should be paused at this point.
	start_gameclock();

	read_objects_dat();
	read_pcs_dat();
	read_pcequip_dat();
	read_npcs_dat();
	read_npcequip_dat();
	read_items_dat();
	read_multis_dat();
	read_storage_dat();
	read_resources_dat();
	read_guilds_dat();
	read_datastore_dat();
	read_party_dat();

	read_incremental_saves();
	insert_deferred_items();

	register_deleted_serials();
	clear_save_index();

	import_new_data();
//	import_wsc();

	//dave 3/9/3
	if( stored_last_item_serial < GetCurrentItemSerialNumber() )
		SetCurrentItemSerialNumber( stored_last_item_serial );
	if( stored_last_char_serial < GetCurrentCharSerialNumber() )
		SetCurrentCharSerialNumber( stored_last_char_serial );

	while (!parent_conts.empty())
		parent_conts.pop();

	for( ObjectHash::hs::const_iterator citr = objecthash.begin(), citrend=objecthash.end(); citr != citrend; ++citr )
	{
		UObject* obj = (*citr).second.get();
		if (obj->ismobile())
		{
			Character* chr = static_cast<Character*>(obj);

			if (chr->acct != NULL)
				chr->logged_in = false;
		}
	}

	gflag_in_system_load = false;
	return 0;
}



SaveContext::SaveContext() :
	_pol(),
	_objects(),
	_pcs(),
	_pcequip(),
	_npcs(),
	_npcequip(),
	_items(),
	_multis(),
	_storage(),
	_resource(),
	_guilds(),
	_datastore(),
	_party(),
	pol(&_pol),
	objects(&_objects),
	pcs(&_pcs),
	pcequip(&_pcequip),
	npcs(&_npcs),
	npcequip(&_npcequip),
	items(&_items),
	multis(&_multis),
	storage(&_storage),
	resource(&_resource),
	guilds(&_guilds),
	datastore(&_datastore),
	party(&_party)
{
	pol.init(config.world_data_path + "pol.ndt");
	objects.init(config.world_data_path + "objects.ndt");
	pcs.init(config.world_data_path + "pcs.ndt");
	pcequip.init(config.world_data_path + "pcequip.ndt");
	npcs.init(config.world_data_path + "npcs.ndt");
	npcequip.init(config.world_data_path + "npcequip.ndt");
	items.init(config.world_data_path + "items.ndt");
	multis.init(config.world_data_path + "multis.ndt");
	storage.init(config.world_data_path + "storage.ndt");
	resource.init(config.world_data_path + "resource.ndt");
	guilds.init(config.world_data_path + "guilds.ndt");
	datastore.init(config.world_data_path + "datastore.ndt");
	party.init(config.world_data_path + "parties.ndt");

	pcs()
			<< "#" << pf_endl
			<< "#  PCS.TXT: Player-Character Data" << pf_endl
			<< "#" << pf_endl
			<< "#  In addition to PC data, this also contains hair, beards, death shrouds," << pf_endl
			<< "#  and backpacks, but not the contents of each backpack." << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	pcequip()
			<< "#" << pf_endl
			<< "#  PCEQUIP.TXT: Player-Character Equipment Data" << pf_endl
			<< "#" << pf_endl
			<< "#  This file can be deleted to wipe all items held/equipped by characters" << pf_endl
			<< "#  Note that hair, beards, empty backpacks, and death shrouds are in PCS.TXT." << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	npcs()
			<< "#" << pf_endl
			<< "#  NPCS.TXT: Nonplayer-Character Data" << pf_endl
			<< "#" << pf_endl
			<< "#  If you delete this file to perform an NPC wipe," << pf_endl
			<< "#  be sure to also delete NPCEQUIP.TXT" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	npcequip()
			<< "#" << pf_endl
			<< "#  NPCEQUIP.TXT: Nonplayer-Character Equipment Data" << pf_endl
			<< "#" << pf_endl
			<< "#  Delete this file along with NPCS.TXT to perform an NPC wipe" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	items()
			<< "#" << pf_endl
			<< "#  ITEMS.TXT: Item data" << pf_endl
			<< "#" << pf_endl
			<< "#  This file also contains ship and house components (doors, planks etc)" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	multis()
			<< "#" << pf_endl
			<< "#  MULTIS.TXT: Ship and House data" << pf_endl
			<< "#" << pf_endl
			<< "#  Deleting this file will not properly wipe houses and ships," << pf_endl
			<< "#  because doors, planks, and tillermen will be left in the world." << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	storage()
			<< "#" << pf_endl
			<< "#  STORAGE.TXT: Contains bank boxes, vendor inventories, and other data." << pf_endl
			<< "#" << pf_endl
			<< "#  This file can safely be deleted to wipe bank boxes and vendor inventories." << pf_endl
			<< "#  Note that scripts may use this for other types of storage as well" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	resource()
			<< "#" << pf_endl
			<< "#  RESOURCE.TXT: Resource System Data" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	guilds()
			<< "#" << pf_endl
			<< "#  GUILDS.TXT: Guild Data" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;

	datastore()
			<< "#" << pf_endl
			<< "#  DATASTORE.TXT: DataStore Data" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;
	party()
			<< "#" << pf_endl
			<< "#  PARTIES.TXT: Party Data" << pf_endl
			<< "#" << pf_endl
			<< pf_endl;
}



void write_global_properties( StreamWriter& sw )
{
	sw()
		<< "GlobalProperties" << pf_endl
		<< "{" << pf_endl;
	global_properties.printProperties( sw );
	sw()
		<< "}" << pf_endl
		<< pf_endl;
	sw.flush();
}

void write_system_data( StreamWriter& sw )
{
	sw()
		<< "System" << pf_endl
		<< "{" << pf_endl
		<< "\tCoreVersion\t" << progver << pf_endl
		<< "\tCoreVersionString\t" << polverstr << pf_endl
		<< "\tCompileDate\t" << compiledate << pf_endl
		<< "\tCompileTime\t" << compiletime << pf_endl
		<< "\tLastItemSerialNumber\t" << GetCurrentItemSerialNumber() << pf_endl //dave 3/9/3
		<< "\tLastCharSerialNumber\t" << GetCurrentCharSerialNumber() << pf_endl //dave 3/9/3
		<< "}" << pf_endl
		<< pf_endl;
	sw.flush();
}

void write_shadow_realms( StreamWriter& sw )
{
	for(const auto &realm : *Realms)
	{
		if( realm->is_shadowrealm )
		{
			sw ()
				<< "Realm" << pf_endl
				<< "{" << pf_endl
				<< "\tName\t" << realm->shadowname << pf_endl
				<< "\tBaseRealm\t" << realm->baserealm->name() << pf_endl
				<< "}" << pf_endl
				<< pf_endl;
		}
	}
	sw.flush();
}

// Austin (Oct. 17, 2006)
// Added to handle gotten item saving.
inline void WriteGottenItem(Character* chr, SaveContext& sc)
{
	Item* item = chr->gotten_item;
	// For now, it just saves the item in items.txt 
	item->x = chr->x;
	item->y = chr->y;
	item->z = chr->z;
	item->realm = chr->realm;

	item->printOn(sc.items);

	item->x = item->y = item->z = 0;
}

void write_characters( SaveContext& sc )
{
	for( const auto &objitr : objecthash )
	{
		UObject* obj = objitr.second.get();
		if (obj->ismobile() && !obj->orphan())
		{
			Character* chr = static_cast<Character*>(obj);
			if (!chr->isa( UObject::CLASS_NPC ))
			{
				chr->printOn( sc.pcs );
				chr->clear_dirty();
				chr->printWornItems( sc.pcs, sc.pcequip );

				// Figure out where to save the 'gotten item' - Austin (Oct. 17, 2006)
				if ( chr->gotten_item && !chr->gotten_item->orphan() )
					WriteGottenItem(chr, sc);
			}
		}
	}
}

void write_npcs( SaveContext& sc )
{
	for( const auto &objitr : objecthash )
	{
		UObject* obj = objitr.second.get();
		if (obj->ismobile() && !obj->orphan())
		{
			Character* chr = static_cast<Character*>(obj);
			if (chr->isa( UObject::CLASS_NPC ))
			{
				if (chr->saveonexit())
				{
					chr->printOn( sc.npcs );
					chr->clear_dirty();
					chr->printWornItems( sc.npcs, sc.npcequip );
				}
			}
		}
	}
}

void write_items( StreamWriter& sw_items )
{
	for( const auto &realm : *Realms )
	{
		unsigned wgridx = realm->width() / WGRID_SIZE;
		unsigned wgridy = realm->height() / WGRID_SIZE;

	// Tokuno-Fix
	if (wgridx * WGRID_SIZE < realm->width())
	  wgridx++;
	if (wgridy * WGRID_SIZE < realm->height())
	  wgridy++;
	
		for( unsigned wx = 0; wx < wgridx; ++wx )
		{
			for( unsigned wy = 0; wy < wgridy; ++wy )
			{
				for( const auto &item : realm->zone[wx][wy].items )
				{
					if (!dont_save_itemtype(item->objtype_) && item->saveonexit())
					{
						sw_items << *item;
						item->clear_dirty();
					}
				}
			}
		}
	}
}

void write_multis( StreamWriter& ofs )
{
	for( const auto &realm : *Realms )
	{
		unsigned wgridx = realm->width() / WGRID_SIZE;
		unsigned wgridy = realm->height() / WGRID_SIZE;

	// Tokuno-Fix
	if (wgridx * WGRID_SIZE < realm->width())
	  wgridx++;
	if (wgridy * WGRID_SIZE < realm->height())
	  wgridy++;
	
		for( unsigned wx = 0; wx < wgridx; ++wx )
		{
			for( unsigned wy = 0; wy < wgridy; ++wy )
			{
				for( auto & multi : realm->zone[wx][wy].multis )
				{
					if (exit_signalled) // drop waiting commit on shutdown
					{
						UHouse* house = multi->as_house();
						if(house != NULL)
						{
							if (house->IsCustom())
							{
								if (house->IsWaitingForAccept())
									house->AcceptHouseCommit(NULL,false);
							}
						}
					}
					ofs << *multi;
					multi->clear_dirty();
				}
			}
		}
	}
}

bool commit( const string& basename )
{
	string bakfile = config.world_data_path + basename + ".bak";
	string datfile = config.world_data_path + basename + ".txt";
	string ndtfile = config.world_data_path + basename + ".ndt";
	
	bool any = false;

	if (FileExists( bakfile ))
	{
		any = true;
		if (unlink( bakfile.c_str() ))
		{
			int err = errno;
			Log2( "Unable to remove %s: %s (%d)\n", bakfile.c_str(), strerror(err), err );
		}
	}
	if (FileExists( datfile ))
	{
		any = true;
		if (rename( datfile.c_str(), bakfile.c_str() ))
		{
			int err = errno;
			Log2( "Unable to rename %s to %s: %s (%d)\n", datfile.c_str(), bakfile.c_str(), strerror(err), err );
		}
	}
	if (FileExists( ndtfile ))
	{
		any = true;
		if (rename( ndtfile.c_str(), datfile.c_str() ))
		{
			int err = errno;
			Log2( "Unable to rename %s to %s: %s (%d)\n", ndtfile.c_str(), datfile.c_str(), strerror(err), err );
		}
	}

	return any;
}

bool should_write_data()
{
	if (config.inhibit_saves)
		return false;
	if (passert_shutdown_due_to_assertion && passert_nosave)
		return false;

	return true;
}

int write_data( unsigned int& dirty_writes, unsigned int& clean_writes, long long& elapsed_ms )
{
	if (!should_write_data())
	{
		dirty_writes = clean_writes = elapsed_ms = 0;
		return -1;
	}

	UObject::dirty_writes = 0;
	UObject::clean_writes = 0;

	vector<long long> times;
	Tools::Timer<> timer;

	{
		SaveContext sc;

		{
			{
				sc.pol()
					<< "#" << pf_endl
					<< "#  Created by Version: " << polverstr << pf_endl
					<< "#  Mobiles:		 " << get_mobile_count() << pf_endl
					<< "#  Top-level Items: " << get_toplevel_item_count() << pf_endl
					<< "#" << pf_endl
					<< pf_endl;


				write_system_data( sc.pol );
				write_global_properties( sc.pol );
				write_shadow_realms( sc.pol );
				sc.pol.flush_file();
			}

			{
				write_items( sc.items );
				sc.items.flush_file();
			}

			{
				write_characters( sc ); // this writes wornitems into items.txt, can they safely be moved into pcequip? 
				sc.pcs.flush_file();
				sc.pcequip.flush_file();
			}

			{
				write_npcs( sc );
				sc.npcs.flush_file();
				sc.npcequip.flush_file();
			}

			{
				write_multis( sc.multis );
				sc.multis.flush_file();
			}

			{
				storage.print(sc.storage);
				sc.storage.flush_file();
			}

			{
				write_resources_dat( sc.resource );
				sc.resource.flush_file();
			}

			{
				write_guilds( sc.guilds );
				sc.guilds.flush_file();
			}

			{
				write_datastore( sc.datastore );
				sc.datastore.flush_file();
				// Atomically (hopefully) perform the switch.
				commit_datastore();
			}

			{
				write_party( sc.party );
				sc.party.flush_file();
			}

			{
				if (accounts_txt_dirty)
				{
					write_account_data();
				}
			}
		}
		times.push_back(timer.ellapsed());
	}

	commit( "pol" );
	commit( "objects" );
	commit( "pcs" );
	commit( "pcequip" );
	commit( "npcs" );
	commit( "npcequip" );
	commit( "items" );
	commit( "multis" );
	commit( "storage" );
	commit( "resource" );
	commit( "guilds" );
	commit( "datastore" );
	commit( "parties" );
	times.push_back(timer.ellapsed());

	commit_incremental_saves();
	incremental_save_count = 0;
	timer.stop();
	times.push_back(timer.ellapsed());
	objecthash.ClearDeleted();

	//cout << "times" << endl;
	//for (const auto &time : times)
	//	cout << time << endl;
	//cout << "Clean: " << UObject::clean_writes << " Dirty: " << UObject::dirty_writes << endl;
	clean_writes = UObject::clean_writes;
	dirty_writes = UObject::dirty_writes;
	elapsed_ms = times.back();

	incremental_saves_disabled = false;
	return 0;
}


void read_starting_locations()
{
	ConfigFile cf( "config/startloc.cfg" );

	ConfigElem elem;
	while (cf.read( elem ))
	{
		if (stricmp( elem.type(), "StartingLocation" ) != 0)
		{
			cerr << "Unknown element type in startloc.cfg: " << elem.type() << endl;
			throw runtime_error( "Error in configuration file." );
		}

		std::unique_ptr<StartingLocation> loc( new StartingLocation );
		loc->city = elem.remove_string( "CITY" );
		loc->desc = elem.remove_string( "DESCRIPTION" );
		loc->mapid = elem.remove_ushort( "MAPID", 0 );
		loc->cliloc_desc = elem.remove_unsigned( "CLILOC", 1075072 );

		string coord;
		while( elem.remove_prop( "Coordinate", &coord ))
		{
			int x, y, z;
			if (sscanf( coord.c_str(), "%d,%d,%d", &x, &y, &z ) == 3)
			{
				loc->coords.push_back( Coordinate(static_cast<u16>(x),static_cast<u16>(y),static_cast<s8>(z)) );
			}
			else
			{
				cerr << "Poorly formed coordinate in startloc.cfg: '"
					 << coord
					 << "' for city "
					 << loc->city
					 << ", description "
					 << loc->desc
					 << endl;
				throw runtime_error( "Configuration file error in startloc.cfg." );
			}
		}
		if (loc->coords.size() == 0)
		{
			cerr << "STARTLOC.CFG: StartingLocation ("
				 << loc->city 
				 << ","
				 << loc->desc
				 << ") has no Coordinate properties."
				 << endl;
			throw runtime_error( "Configuration file error." );
		}
		startlocations.push_back( loc.release() );
	}

	if (startlocations.empty())
		throw runtime_error( "STARTLOC.CFG: No starting locations found.  Clients will crash on character creation." );
}

ServerDescription::ServerDescription() :
	name(""),
	port(0),
	hostname("")
{
	memset( ip, 0, sizeof ip );
}

void read_gameservers()
{
	string accttext;

	ConfigFile cf( "config/servers.cfg" );

	ConfigElem elem;
	while (cf.read( elem ))
	{
		if (!elem.type_is( "GameServer" ))
			continue;

		std::unique_ptr<ServerDescription> svr( new ServerDescription );
		
		svr->name = elem.remove_string( "NAME" );
		
		string iptext;
		int ip0, ip1, ip2, ip3;
		iptext = elem.remove_string( "IP" );
		if (iptext == "--ip--")
		{
			iptext = ipaddr_str;
			if (iptext == "")
			{
				cout << "Skipping server " << svr->name << " because there is no Internet IP address." << endl;
				continue;
			}
		}
		else if (iptext == "--lan--")
		{
			iptext = lanaddr_str;
			if (iptext == "")
			{
				cout << "Skipping server " << svr->name << " because there is no LAN IP address." << endl;
				continue;
			}
		}
		
		if (isdigit(iptext[0]))
		{
			if (sscanf( iptext.c_str(), "%d.%d.%d.%d", &ip0, &ip1, &ip2, &ip3 ) != 4)
			{
				cerr << "SERVERS.CFG: Poorly formed IP ("
						<< iptext 
						<< ") for GameServer '"
						<< svr->name 
						<< "'." 
						<< endl;
				throw runtime_error( "Configuration file error." );
			}
			svr->ip[0] = static_cast<unsigned char>(ip3);
			svr->ip[1] = static_cast<unsigned char>(ip2);
			svr->ip[2] = static_cast<unsigned char>(ip1);
			svr->ip[3] = static_cast<unsigned char>(ip0);
		}
		else
		{
			svr->hostname = iptext;

#ifdef __linux__
			/* try to look up */
			struct hostent host_ret;
			struct hostent* host_result = NULL;
			char tmp_buf[ 1024 ];
			int my_h_errno;
			int res = gethostbyname_r( svr->hostname.c_str(), &host_ret, tmp_buf, sizeof tmp_buf, &host_result, &my_h_errno );
			if (res == 0 && host_result && host_result->h_addr_list[0])
			{
				char* addr = host_result->h_addr_list[0];
				svr->ip[0] = addr[3];
				svr->ip[1] = addr[2];
				svr->ip[2] = addr[1];
				svr->ip[3] = addr[0];
/*
				struct sockaddr_in saddr;
				memcpy( &saddr.sin_addr, he->h_addr_list[0], he->h_length);
				server->ip[0] = saddr.sin_addr.S_un.S_un_b.s_b1;
				server->ip[1] = saddr.sin_addr.S_un.S_un_b.s_b2;
				server->ip[2] = saddr.sin_addr.S_un.S_un_b.s_b3;
				server->ip[3] = saddr.sin_addr.S_un.S_un_b.s_b4;
*/
			}
			else
			{
				Log2( "Warning: gethostbyname_r failed for server %s (%s): %d\n",
						svr->name.c_str(), svr->hostname.c_str(), my_h_errno );
			}
#endif

		}
		
		svr->port = elem.remove_ushort( "PORT" );

		while (elem.remove_prop( "IPMATCH", &iptext ))
		{
			string::size_type delim = iptext.find_first_of( "/" );
			if (delim != string::npos)
			{
				string ipaddr_str = iptext.substr( 0, delim );
				string ipmask_str = iptext.substr( delim+1 );
				unsigned int ipaddr = inet_addr( ipaddr_str.c_str() );
				unsigned int ipmask = inet_addr( ipmask_str.c_str() );
				svr->ip_match.push_back( ipaddr );
				svr->ip_match_mask.push_back( ipmask );
			}
			else
			{
				unsigned int ipaddr = inet_addr( iptext.c_str() );
				svr->ip_match.push_back( ipaddr );
				svr->ip_match_mask.push_back( 0xFFffFFffLu );
			}
		}

		while (elem.remove_prop( "ACCTMATCH", &accttext ))
		{
			svr->acct_match.push_back(accttext);
		}

		servers.push_back( svr.release() );
	}
	if (servers.empty())
		throw runtime_error( "There must be at least one GameServer in SERVERS.CFG." );
}



