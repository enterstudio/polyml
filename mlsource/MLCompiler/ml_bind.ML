(*
    Copyright David C.J. Matthews 2016

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*)
structure Boot =
struct
    structure Address        = Address
    structure Misc           = Misc
    structure HashTable      = HashTable
    structure UniversalTable = UniversalTable
    structure StretchArray   = StretchArray
end;

structure MLCompiler =
struct
    local
        (* Force Initialise to be built when using PolyML.make.
           This builds pretty well everything. *)
        structure S = Initialise
    in
        (* Build Make. *)
        open Make
    end
end;
