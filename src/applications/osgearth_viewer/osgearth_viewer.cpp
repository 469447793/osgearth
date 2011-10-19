/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2010 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include <osg/Notify>
#include <osgGA/StateSetManipulator>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>
#include <osgEarth/MapNode>
#include <osgEarth/XmlUtils>
#include <osgEarthUtil/EarthManipulator>
#include <osgEarthUtil/AutoClipPlaneHandler>
#include <osgEarthUtil/Controls>
#include <osgEarthUtil/Graticule>
#include <osgEarthUtil/SkyNode>
#include <osgEarthUtil/Viewpoint>
#include <osgEarthUtil/Formatters>
#include <osgEarthUtil/Annotation>
#include <osgEarthSymbology/Color>
#include <osgEarthDrivers/kml/KML>
#include <osgEarthDrivers/ocean_surface/OceanSurface>

using namespace osgEarth::Util;
using namespace osgEarth::Util::Annotation;
using namespace osgEarth::Util::Controls;
using namespace osgEarth::Symbology;
using namespace osgEarth::Drivers;

int
usage( const std::string& msg )
{
    OE_NOTICE << msg << std::endl;
    OE_NOTICE << std::endl;
    OE_NOTICE << "USAGE: osgearth_viewer [options] file.earth" << std::endl;
    OE_NOTICE << "   --sky           : activates the atmospheric model" << std::endl;
    OE_NOTICE << "   --autoclip      : activates the auto clip-plane handler" << std::endl;
    OE_NOTICE << "   --dms           : format coordinates as degrees/minutes/seconds" << std::endl;
    OE_NOTICE << "   --mgrs          : format coordinates as MGRS" << std::endl;
    OE_NOTICE << "   --ocean         : display an ocean surface layer, if found" << std::endl;   
        
    return -1;
}

struct PrintMe : public osg::NodeCallback {
    void operator()(osg::Node* node, osg::NodeVisitor* nv) {
        traverse(node,nv);
        osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(nv);
        double n, f, a, v;
        cv->getProjectionMatrix()->getPerspective(v, a, n, f);
        OE_NOTICE << std::fixed << "near = " << n << ", far = " << f << ", ratio = " << n/f << std::endl;
    }
};

static EarthManipulator* s_manip         =0L;
static Control*          s_controlPanel  =0L;
static SkyNode*          s_sky           =0L;
static bool              s_dms           =false;
static bool              s_mgrs          =false;
static bool              s_ocean         =false;

struct SkySliderHandler : public ControlEventHandler
{
    virtual void onValueChanged( class Control* control, float value )
    {
        s_sky->setDateTime( 2011, 3, 6, value );
    }
};

struct ToggleNodeHandler : public ControlEventHandler
{
    ToggleNodeHandler( osg::Node* node ) : _node(node) { }

    virtual void onValueChanged( class Control* control, bool value )
    {
        osg::ref_ptr<osg::Node> safeNode = _node.get();
        if ( safeNode.valid() )
            safeNode->setNodeMask( value ? ~0 : 0 );
    }

    osg::observer_ptr<osg::Node> _node;
};

struct ClickViewpointHandler : public ControlEventHandler
{
    ClickViewpointHandler( const Viewpoint& vp ) : _vp(vp) { }
    Viewpoint _vp;

    virtual void onClick( class Control* control )
    {
        s_manip->setViewpoint( _vp, 4.5 );
    }
};

struct MouseCoordsHandler : public osgGA::GUIEventHandler
{
    MouseCoordsHandler( LabelControl* label, osgEarth::MapNode* mapNode )
        : _label( label ),
          _mapNode( mapNode )
    {
        _mapNodePath.push_back( mapNode->getTerrainEngine() );
    }

    bool handle( const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa )
    {
        osgViewer::View* view = static_cast<osgViewer::View*>(aa.asView());
        if (ea.getEventType() == ea.MOVE || ea.getEventType() == ea.DRAG)
        {
            osgUtil::LineSegmentIntersector::Intersections results;
            if ( view->computeIntersections( ea.getX(), ea.getY(), _mapNodePath, results ) )
            {
                // find the first hit under the mouse:
                osgUtil::LineSegmentIntersector::Intersection first = *(results.begin());
                osg::Vec3d point = first.getWorldIntersectPoint();
                osg::Vec3d lla;

                // transform it to map coordinates:
                _mapNode->getMap()->worldPointToMapPoint(point, lla);

                std::stringstream ss;

                if ( s_mgrs )
                {
                    MGRSFormatter f( MGRSFormatter::PRECISION_1M );
                    ss << "MGRS: " << f.format(lla.y(), lla.x()) << "   ";
                }
                 // lat/long
                {
                    LatLongFormatter::AngularFormat fFormat = s_dms?
                        LatLongFormatter::FORMAT_DEGREES_MINUTES_SECONDS :
                        LatLongFormatter::FORMAT_DECIMAL_DEGREES;
                    
                    LatLongFormatter f( fFormat );

                    ss 
                        << "Lat: " << f.format( Angular(lla.y(),Units::DEGREES), 4 ) << "  "
                        << "Lon: " << f.format( Angular(lla.x(),Units::DEGREES), 5 );
                }

                _label->setText( ss.str() );
            }
            else
            {
                //Clear the text
                _label->setText( "" );
            }
        }
        return false;
    }

    osg::ref_ptr< LabelControl > _label;
    MapNode*                     _mapNode;
    osg::NodePath                _mapNodePath;
};



void
createControlPanel( osgViewer::View* view, std::vector<Viewpoint>& vps )
{
    ControlCanvas* canvas = ControlCanvas::get( view );

    VBox* main = new VBox();
    main->setBackColor(0,0,0,0.5);
    main->setMargin( 10 );
    main->setPadding( 10 );
    main->setChildSpacing( 10 );
    main->setAbsorbEvents( true );
    main->setVertAlign( Control::ALIGN_BOTTOM );

    if ( vps.size() > 0 )
    {
        // the viewpoint container:
        Grid* g = new Grid();
        g->setChildSpacing( 0 );
        g->setChildVertAlign( Control::ALIGN_CENTER );

        unsigned i;
        for( i=0; i<vps.size(); ++i )
        {
            const Viewpoint& vp = vps[i];
            std::stringstream buf;
            buf << (i+1);
            Control* num = new LabelControl(buf.str(), 16.0f, osg::Vec4f(1,1,0,1));
            num->setPadding( 4 );
            g->setControl( 0, i, num );

            Control* vpc = new LabelControl(vp.getName().empty() ? "<no name>" : vp.getName(), 16.0f);
            vpc->setPadding( 4 );
            vpc->setHorizFill( true );
            vpc->setActiveColor( Color::Blue );
            vpc->addEventHandler( new ClickViewpointHandler(vp) );
            g->setControl( 1, i, vpc );
        }
        main->addControl( g );
    }

    // sky time slider:
    if ( s_sky )
    {
        HBox* skyBox = new HBox();
        skyBox->setChildVertAlign( Control::ALIGN_CENTER );
        skyBox->setChildSpacing( 10 );
        skyBox->setHorizFill( true );

        skyBox->addControl( new LabelControl("Time: ", 16) );

        HSliderControl* skySlider = new HSliderControl( 0.0f, 24.0f, 18.0f );
        skySlider->setBackColor( Color::Gray );
        skySlider->setHeight( 12 );
        skySlider->setHorizFill( true, 200 );
        skySlider->addEventHandler( new SkySliderHandler );
        skyBox->addControl( skySlider );

        main->addControl( skyBox );
    }
    
    canvas->addControl( main );

    s_controlPanel = main;
}

/**
 * Visitor that builds a UI control for a loaded KML file.
 */
struct KMLUIBuilder : public osg::NodeVisitor
{
    KMLUIBuilder( ControlCanvas* canvas ) : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN), _canvas(canvas)
    {
        _grid = new Grid();
        _grid->setAbsorbEvents( true );
        _grid->setPadding( 5 );
        _grid->setVertAlign( Control::ALIGN_TOP );
        _grid->setHorizAlign( Control::ALIGN_LEFT );
        _grid->setBackColor( Color(Color::Black,0.5) );
        _canvas->addControl( _grid );
    }

    void apply( osg::Node& node )
    {
        AnnotationData* data = dynamic_cast<AnnotationData*>( node.getUserData() );
        if ( data )
        {
            ControlVector row;
            CheckBoxControl* cb = new CheckBoxControl( node.getNodeMask() != 0, new ToggleNodeHandler( &node ) );
            cb->setSize( 12, 12 );
            row.push_back( cb );
            std::string name = data->getName().empty() ? "<unnamed>" : data->getName();
            LabelControl* label = new LabelControl( name, 14.0f );
            label->setMargin(Gutter(0,0,0,(this->getNodePath().size()-3)*20));
            if ( data->getViewpoint() )
            {
                label->addEventHandler( new ClickViewpointHandler(*data->getViewpoint()) );
                label->setActiveColor( Color::Blue );
            }
            row.push_back( label );
            _grid->addControls( row );
        }
        traverse(node);
    }

    ControlCanvas* _canvas;
    Grid*          _grid;
};

void addMouseCoords(osgViewer::Viewer* viewer, osgEarth::MapNode* mapNode)
{
    ControlCanvas* canvas = ControlCanvas::get( viewer );
    LabelControl* mouseCoords = new LabelControl();
    mouseCoords->setHorizAlign(Control::ALIGN_CENTER );
    mouseCoords->setVertAlign(Control::ALIGN_BOTTOM );
    mouseCoords->setBackColor(0,0,0,0.5);    
    mouseCoords->setSize(400,50);
    mouseCoords->setMargin( 10 );
    canvas->addControl( mouseCoords );

    viewer->addEventHandler( new MouseCoordsHandler(mouseCoords, mapNode ) );
}

struct ViewpointHandler : public osgGA::GUIEventHandler
{
    ViewpointHandler( const std::vector<Viewpoint>& viewpoints )
        : _viewpoints( viewpoints ) { }

    bool handle( const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa )
    {
        if ( ea.getEventType() == ea.KEYDOWN )
        {
            int index = (int)ea.getKey() - (int)'1';
            if ( index >= 0 && index < (int)_viewpoints.size() )
            {
                s_manip->setViewpoint( _viewpoints[index], 4.5 );
            }
            else if ( ea.getKey() == 'v' )
            {
                Viewpoint vp = s_manip->getViewpoint();
                XmlDocument xml( vp.getConfig() );
                xml.store( std::cout );
                std::cout << std::endl;
            }
            else if ( ea.getKey() == '?' )
            {
                s_controlPanel->setVisible( !s_controlPanel->visible() );
            }
        }
        return false;
    }

    std::vector<Viewpoint> _viewpoints;
};

int
main(int argc, char** argv)
{
    osg::ArgumentParser arguments(&argc,argv);
    osg::DisplaySettings::instance()->setMinimumNumStencilBits( 8 );
    osgViewer::Viewer viewer(arguments);

    bool useAutoClip  = arguments.read( "--autoclip" );
    bool useSky       = arguments.read( "--sky" );
    s_ocean           = arguments.read( "--ocean" );
    s_dms             = arguments.read( "--dms" );
    s_mgrs            = arguments.read( "--mgrs" );

    std::string kmlFile;
    arguments.read( "--kml", kmlFile );

    // load the .earth file from the command line.
    osg::Node* earthNode = osgDB::readNodeFiles( arguments );
    if (!earthNode)
        return usage( "Unable to load earth model." );
    
    s_manip = new EarthManipulator();
    s_manip->getSettings()->setArcViewpointTransitions( true );
    viewer.setCameraManipulator( s_manip );

    osg::Group* root = new osg::Group();
    root->addChild( earthNode );

    // create a graticule and clip plane handler.
    Graticule* graticule = 0L;
    osgEarth::MapNode* mapNode = osgEarth::MapNode::findMapNode( earthNode );
    if ( mapNode )
    {
        const Config& externals = mapNode->externalConfig();

        if ( mapNode->getMap()->isGeocentric() )
        {
            // Sky model.
            Config skyConf = externals.child( "sky" );
            if ( !skyConf.empty() )
                useSky = true;

            if ( useSky )
            {
                double hours = skyConf.value( "hours", 12.0 );
                s_sky = new SkyNode( mapNode->getMap() );
                s_sky->setDateTime( 2011, 3, 6, hours );
                s_sky->attach( &viewer );
                root->addChild( s_sky );
            }

            // Ocean surface.
            if ( s_ocean )
            {
                osg::Node* oceanNode = OceanSurface::loadOceanSurface( mapNode->getMap() );
                root->addChild( oceanNode );
            }

            if ( externals.hasChild("autoclip") )
            {
                useAutoClip = externals.child("autoclip").boolValue( useAutoClip );
            }

            // the AutoClipPlaneCallback will automatically adjust the near/far clipping
            // planes based on your view of the horizon. This prevents near clipping issues
            // when you are very close to the ground.
            if ( s_sky || s_ocean || useAutoClip )
            {
                viewer.getCamera()->addCullCallback( new AutoClipPlaneCallback(mapNode->getMap()) );
            }
        }

        // read in viewpoints, if any
        std::vector<Viewpoint> viewpoints;
        const ConfigSet children = externals.children("viewpoint");
        if ( children.size() > 0 )
        {
            for( ConfigSet::const_iterator i = children.begin(); i != children.end(); ++i )
                viewpoints.push_back( Viewpoint(*i) );

            viewer.addEventHandler( new ViewpointHandler(viewpoints) );
        }

        // Add a control panel to the scene
        root->addChild( ControlCanvas::get( &viewer ) );
        if ( viewpoints.size() > 0 || s_sky )
            createControlPanel(&viewer, viewpoints);

        addMouseCoords( &viewer, mapNode );

        // Load a KML file if specified
        if ( !kmlFile.empty() )
        {
            KMLOptions kmlo;
            kmlo.defaultIconImage() = URI("http://www.osgearth.org/chrome/site/pushpin_yellow.png").readImage();

            osg::Node* kml = KML::load( URI(kmlFile), mapNode, kmlo );
            if ( kml )
            {
                root->addChild( kml );

                KMLUIBuilder uibuilder( ControlCanvas::get(&viewer) );
                root->accept( uibuilder );                
            }
        }
    }

    // osgEarth benefits from pre-compilation of GL objects in the pager. In newer versions of
    // OSG, this activates OSG's IncrementalCompileOpeartion in order to avoid frame breaks.
    viewer.getDatabasePager()->setDoPreCompile( true );

    viewer.setSceneData( root );

    // add some stock OSG handlers:
    viewer.addEventHandler(new osgViewer::StatsHandler());
    viewer.addEventHandler(new osgViewer::WindowSizeHandler());
    viewer.addEventHandler(new osgViewer::ThreadingHandler());
    viewer.addEventHandler(new osgViewer::LODScaleHandler());
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));

    //viewer.getCamera()->addCullCallback( new PrintMe() );

    return viewer.run();
}
