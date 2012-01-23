# implementation of sandwich kingdom content specification

from sandwich_map import *
from sandwich_dialog import *
from sandwich_script import *

import lxml.etree
import os, os.path, re, traceback, sys, zlib
import tmx, misc

def load():
	try:
		return World(".")
	except:
		log_error()

def export():
	try:
		World(".").export()
		content_hash = crc([ "content.gen.lua", "content.gen.cpp" ])
		print "CONTENT CRC = " + content_hash
		with open("content_crc.txt", "w") as f: 
			f.write(content_hash)
	except:
		log_error()
		sys.exit(-1)

def log_error():
	typ,val,bt = sys.exc_info()
	print "\n\nUnexpected error:", val
	print "-----------"
	traceback.print_tb(bt)

def crc(fileNames):
    prev = 0
    for fileName in fileNames:
    	for eachLine in open(fileName,"rb"):
        	prev = zlib.crc32(eachLine, prev)
    return "%X"%(prev & 0xFFFFFFFF)

class World:
	def __init__(self, dir):
		self.dir = dir
		self.script = GameScript(self, os.path.join(dir, "game-script.xml"))
		self.dialog = DialogDatabase(self, os.path.join(dir, "dialog-database.xml"))

		# list maps in alphabetical order
		maps_by_name = [ (map.id, map) for map in (Map(self, os.path.join(dir, path)) for path in os.listdir(dir) if path.endswith(".tmx")) ]
		maps_by_name.sort();
		self.maps = [ map for _,map in maps_by_name ]
		self.map_dict = dict(maps_by_name)
		for i,m in enumerate(self.maps):
			m.index = i
		
		#validate maps
		assert len(self.maps) > 0
		# validate map links
		for map in self.maps:
			for gate in map.list_triggers_of_type(TRIGGER_GATEWAY):
				assert gate.target_map in self.map_dict, "gateway to unknown map: " + gate.target_map
				tmap = self.map_dict[gate.target_map]
				found = False
				for othergate in tmap.list_triggers_of_type(TRIGGER_GATEWAY):
					if othergate.id == gate.target_gate:
						found = True
						break
				assert found, "link to unknown map-gate: " + gate.target_gate
		# validate quests
		if len(self.script.unlockables) > 32:
			raise Exception("More than 32 unlockable flags (implicit and explicit) in game script")
		for quest in self.script.quests:
			assert quest.map in self.map_dict, "unknown map in gamte sript: " + quest.map
			assert 0 <= quest.x and 0 <= quest.y and quest.x < self.map_dict[quest.map].width and quest.y < self.map_dict[quest.map].height, "invalid map starting position"
			assert len(quest.flags) <= 32, "More than 32 flags (implicit and explicit) in quest: " + quest.id
			# validate triggers
			for m in self.maps:
				for r in m.rooms:
					r.validate_triggers_for_quest(quest)


	def export(self):
		with open(os.path.join(self.dir,"content.gen.lua"), "w") as lua:
			lua.write("--GENERATED BY SANDWICH.PY, DO NOT EDIT BY HAND\n")
			lua.write("\n-- MAP IMAGES\n")
			for filename in (os.path.basename(path) for path in os.listdir(self.dir) if path.endswith(".tmx")):
				name = filename[:-4]
				lua.write("TileSet_%s = image{ \"%s.png\", width=16, height=16 }\n" % (name,name))
				if os.path.exists(name + "_overlay.png"):
					lua.write("Overlay_%s = image{ \"%s_overlay.png\", width=16, height=16 }\n" % (name,name))
				lua.write("Blank_%s = image{ \"%s_blank.png\", width=128, height=128 }\n" % (name,name))
			lua.write("\n-- DIALOG IMAGES\n")
			for name in self.dialog.list_npc_image_names():
				lua.write("NPC_%s = image{ \"%s.png\", width=32, height=32, pinned=true }\n" % (name,name))
			for name in self.dialog.list_detail_image_names():
				lua.write("NPC_Detail_%s = image{ \"%s.png\" }\n" % (name,name))
		
		with open(os.path.join(self.dir,"content.gen.cpp"), "w") as src:
			src.write("// GENERATED BY SANDWICH.PY, NO NOT EDIT BY HAND\n")
			src.write("#include \"Content.h\"\n#include \"assets.gen.h\"\n\n")
			src.write("const unsigned gMapCount = %d;\n" % len(self.maps))
			src.write("const unsigned gQuestCount = %d;\n" % len(self.script.quests))
			src.write("const unsigned gDialogCount = %d;\n\n" % len(self.dialog.dialogs))
			for map in self.maps:
				map.write_source_to(src)
			src.write("\nconst MapData gMapData[] = {\n")
			for map in self.maps:
				map.write_decl_to(src)
			src.write("};\n\n")

			src.write("const QuestData gQuestData[] = {\n")
			for q in self.script.quests:
				m = self.map_dict[q.map]
				src.write("    { %s, %s },\n" % (hex(m.index), hex(m.roomat(q.x, q.y).lid)))
			src.write("};\n\n")
			
			for d in self.dialog.dialogs:
				src.write("static const DialogTextData %s_lines[] = {\n" % d.id)
				for l in d.lines:
					src.write("    { &NPC_Detail_%s, \"%s\" },\n" % (l.image, l.text))
				src.write("};\n")
			src.write("\nconst DialogData gDialogData[] = {\n")
			for d in self.dialog.dialogs:
				src.write("    { &NPC_%s, %d, %s_lines },\n" % (d.npc, len(d.lines), d.id))

			src.write("};\n\n")


if __name__ == "__main__": export()
